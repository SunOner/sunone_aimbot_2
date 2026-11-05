#include "nodes/node_graph.h"
#include <tuple>
#include "nodes/node_layout.h"
#include "node_previous_starters.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace
{
using namespace NodeGraph;
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void publish(Service& service,const Workspace& workspace){std::string error;if(!service.publish(workspace,error))throw std::runtime_error(error);}
Node& node(Graph& graph,int id){for(auto& n:graph.nodes)if(n.id==id)return n;throw std::runtime_error("Missing test node");}
void settings(Workspace& workspace,NodeKind kind,const char* key,double value){for(auto& graph:workspace.graphs)for(auto& n:graph.nodes)if(n.kind==kind)n.parameters[key]=value;}
Workspace emptyWorkspace(){Workspace result;for(std::size_t i=0;i<GraphCount;++i)result.graphs[i].kind=static_cast<GraphKind>(i);return result;}
void constant(Graph& graph,int id,double value,bool boolean=false){auto n=defaultNode(boolean?NodeKind::Boolean:NodeKind::Number,id);n.value=value;graph.nodes.push_back(std::move(n));}
struct Fixture
{
    Context context;
    bool observed=true,inScope=true;
    std::map<int,bool> keys;
    int predictions=0,winds=0,selections=0;
    DetectionStream lastSelected;
    Fixture()
    {
        context.aimHeld=true;context.captureFps=30;context.center={20,40};setDetections({{10,20,20,40,0,0.9}});
        context.selectTarget=[this](int key,const DetectionStream& input,bool,const Node& settings) {
            ++selections;if(key<1000001)lastSelected=input;TargetData selected;if(input->objects.empty())return selected;
            const auto& d=*std::min_element(input->objects.begin(),input->objects.end(),[](const Detection& a,const Detection& b){return std::hypot(a.x+a.width*0.5,a.y+a.height*0.5)<std::hypot(b.x+b.width*0.5,b.y+b.height*0.5);});
            selected.x=d.x;selected.y=d.y;selected.width=d.width;selected.height=d.height;selected.point={d.x+d.width*0.5,d.y+d.height*0.5};selected.classId=d.classId;selected.confidence=d.confidence;
            selected.valid=true;selected.observed=observed;selected.fresh=input->fresh;selected.trackId=settings.kind==NodeKind::TargetNearest?-1:42;selected.velocityX=12;selected.velocityY=-3;selected.missedFrames=observed?0:1;selected.ageSeconds=0.01;return selected;
        };
        context.predict=[this](int,Point p,const Node&,double lookahead,bool reset){++predictions;return Point{p.x+(reset?0:lookahead*10),p.y};};
        context.estimateMotion=[this](int,Point p,const Node&,bool reset){++predictions;return MotionData{p,{10,0},0,!reset,!reset};};
        context.windMotion=[this](int,const Movement& m,const Node&){++winds;return m;};
        context.speedCurve=[](Point,Point,double,const Node& n){return parameter(n,"min_speed");};
        context.mouseDelta=[](Point p,double speed,Point center,const Node&){Movement m;m.steps.push_back({static_cast<int>((p.x-center.x)*speed),static_cast<int>((p.y-center.y)*speed)});m.pixelScaleX=2;m.pixelScaleY=3;return m;};
        context.motionCompensation=[](Point p){return p;};context.inScope=[this](const TargetData&,double,Point){return inScope;};context.keyDown=[this](int key){return keys[key];};
    }
    void setDetections(std::vector<Detection> objects){auto frame=std::make_shared<DetectionFrame>();frame->objects=std::move(objects);frame->version=1;frame->generation=7;frame->fresh=true;frame->width=320;frame->height=320;context.detections=frame;}
};
void testDefaultExecution()
{
    Service s;Fixture f;auto w=defaults();settings(w,NodeKind::AutoShootEnabled,"enabled",1);settings(w,NodeKind::RecoilEnabled,"enabled",1);settings(w,NodeKind::RecoilStrength,"strength",4);publish(s,w);
    f.context.shootHeld=f.context.zoomHeld=true;auto a=s.evaluate(f.context);
    require(a.aim&&a.autoShoot&&a.recoil,"Default graph did not drive all three outputs");require(a.aimPoint.x==20&&a.aimPoint.y==26,"Aim point must be calculated from node offsets");require(a.recoilStrength==4,"Recoil strength must belong to its source node");
    require(a.activePredictionKeys.size()==1&&a.activeWindKeys.size()==1,"Stateful nodes must report identities");
    f.context.aimHeld=false;a=s.evaluate(f.context);require(!a.aim&&!a.autoShoot&&a.recoil,"Inactive activation nodes must stop aim and shooting");
    settings(w,NodeKind::AutoAimEnabled,"enabled",1);publish(s,w);require(s.evaluate(f.context).aim,"Node-owned Auto Aim must activate its graph");
    f.inScope=false;f.context.center={1000,1000};require(!s.evaluate(f.context).autoShoot,"Scope node must control auto shoot");f.setDetections({});a=s.evaluate(f.context);require(!a.aim&&!a.autoShoot&&a.recoil,"Empty stream must update target loss without disabling recoil");
}
void testPayloadAndFilters()
{
    Service s;Fixture f;f.setDetections({{10,20,20,40,0,0.9},{100,50,10,20,1,0.8},{0,0,5,5,9,1}});auto a=s.evaluate(f.context);
    require(a.aim&&f.lastSelected->objects.size()==1&&f.lastSelected->objects[0].classId==1,"Class preference must process actual payload");require(a.aimPoint.x==105&&a.aimPoint.y==51,"Head point must use relative offset");
    auto w=defaults();auto& g=w.graphs[0];auto confidence=defaultNode(NodeKind::FilterConfidence,70);confidence.parameters["threshold"]=0.85;g.nodes.push_back(confidence);
    for(auto& l:g.links)if(l.fromNode==30&&l.toNode==31)l.fromNode=70;g.links.push_back({30,0,70,0});publish(s,w);a=s.evaluate(f.context);
    require(a.aim&&a.aimPoint.x==20&&f.lastSelected->objects[0].classId==0,"Confidence filter must affect selected data");require(f.context.detections->objects.size()==3,"Filter must not mutate shared source");
    auto bad=std::make_shared<DetectionFrame>(*f.context.detections);bad->objects[0].confidence=2;f.context.detections=bad;require(!s.evaluate(f.context).aim,"Malformed payload must fail closed");
}
void testPerNodeSettingsAndPorts()
{
    Service s;Fixture f;auto w=defaults();auto& g=w.graphs[0];int time=0,estimate=0;
    for(auto& n:g.nodes){if(n.kind==NodeKind::PredictionTime){time=n.id;n.parameters["lookahead"]=0.25;}if(n.kind==NodeKind::MotionEstimate)estimate=n.id;}
    publish(s,w);require(s.evaluate(f.context).aimPoint.x==22.5,"Detached optional pin must use node parameter");
    constant(g,71,0.5);g.links.push_back({71,0,time,0});publish(s,w);require(s.evaluate(f.context).aimPoint.x==25,"Wired pin must override parameter");constant(g,72,1,true);g.links.push_back({72,0,estimate,1});publish(s,w);require(s.evaluate(f.context).aimPoint.x==20,"Reset pin must reach estimator");
    auto revision=s.revision();node(g,estimate).parameters["kalman_measurement_noise"]=123;publish(s,w);require(s.revision()>revision,"Algorithm settings must change revision");revision=s.revision();node(g,estimate).x+=32;node(g,estimate).collapsed=true;g.groups[0].title="Renamed";publish(s,w);require(s.revision()==revision,"Layout must preserve execution revision");
}
void testBranchActivityAndBypass()
{
    Service s;Fixture f;auto w=defaults();w.graphs[0].nodes.clear();w.graphs[0].links.clear();w.graphs[0].groups.clear();auto& g=w.graphs[1];
    g.nodes.push_back(defaultNode(NodeKind::AimPoint,70));g.nodes.push_back(defaultNode(NodeKind::Prediction,71));g.nodes.push_back(defaultNode(NodeKind::PointX,72));constant(g,73,0);g.nodes.push_back(defaultNode(NodeKind::Greater,74));g.links.insert(g.links.end(),{{8,0,70,0},{70,0,71,0},{71,0,72,0},{72,0,74,0},{73,0,74,1}});
    for(auto& l:g.links)if(l.toNode==6&&l.toPort==0)l.fromNode=74;publish(s,w);auto a=s.evaluate(f.context);
    require(!a.aim&&a.autoShoot&&a.activePredictionKeys==std::vector<int>{1000072},"Other graph predictor must stay active without Aim Output");node(g,71).bypassed=true;publish(s,w);a=s.evaluate(f.context);require(a.autoShoot&&a.activePredictionKeys.empty(),"Bypass must skip stateful callback");node(g,6).bypassed=true;publish(s,w);a=s.evaluate(f.context);require(!a.autoShoot&&a.activeSelectionKeys.empty(),"Bypassed sink must skip dependencies");
}
void testKeyEvents()
{
    Fixture f;Service s;auto w=emptyWorkspace();auto& g=w.graphs[2];auto key=defaultNode(NodeKind::KeyPressed,1);key.parameters["virtual_key"]=65;g.nodes.push_back(key);constant(g,2,1);g.nodes.push_back(defaultNode(NodeKind::RecoilOutput,3));g.links={{1,0,3,0},{2,0,3,1}};publish(s,w);
    require(!s.evaluate(f.context).recoil,"Unpressed key must be false");f.keys[65]=true;require(s.evaluate(f.context).recoil,"KeyPressed must detect rising edge");require(!s.evaluate(f.context).recoil,"KeyPressed must not repeat while held");f.keys[65]=false;s.evaluate(f.context);f.keys[65]=true;require(s.evaluate(f.context).recoil,"KeyPressed must rearm");
    g.nodes[0].kind=NodeKind::KeyHeld;g.nodes.push_back(defaultNode(NodeKind::Toggle,4));g.links[0].fromNode=4;g.links.push_back({1,0,4,0});publish(s,w);require(s.evaluate(f.context).recoil&&s.evaluate(f.context).recoil,"Toggle must latch while held");f.keys[65]=false;require(s.evaluate(f.context).recoil,"Toggle must stay latched after release");f.keys[65]=true;require(!s.evaluate(f.context).recoil,"Next edge must unlatch");
}
void testCapturedInputDoesNotCreateEdges()
{
    using K=NodeKind;
    for(const auto kind:{K::KeyPressed,K::KeyReleased})
    {
        Fixture f;Service service;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[2];
        auto key=defaultNode(kind,1);key.parameters["virtual_key"]=65;graph.nodes.push_back(key);
        constant(graph,2,1);graph.nodes.push_back(defaultNode(K::RecoilOutput,3));
        graph.links={{1,0,3,0},{2,0,3,1}};publish(service,workspace);
        f.keys[65]=true;service.evaluate(f.context);
        f.context.inputCaptured=true;f.keys[65]=false;
        require(!service.evaluate(f.context).recoil,"Captured input must disable operational sinks");
        f.context.inputCaptured=false;
        require(!service.evaluate(f.context).recoil,"Leaving the editor must not synthesize a release edge");
        f.context.inputCaptured=true;f.keys[65]=true;service.evaluate(f.context);
        f.context.inputCaptured=false;
        require(!service.evaluate(f.context).recoil,"Leaving the editor must not synthesize a press edge");
        f.keys[65]=false;const bool released=service.evaluate(f.context).recoil;
        f.keys[65]=true;const bool pressed=service.evaluate(f.context).recoil;
        require(kind==K::KeyPressed?pressed:released,"Real key edges must rearm after capture");
    }
    Fixture f;Service service;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[2];
    auto key=defaultNode(K::KeyHeld,1);key.parameters["virtual_key"]=65;
    graph.nodes={key,defaultNode(K::Toggle,2),defaultNode(K::RecoilOutput,4)};
    constant(graph,3,1);graph.links={{1,0,2,0},{2,0,4,0},{3,0,4,1}};publish(service,workspace);
    require(!service.evaluate(f.context).recoil,"Toggle must begin off");
    f.context.inputCaptured=true;f.keys[65]=true;service.evaluate(f.context);
    f.context.inputCaptured=false;
    require(!service.evaluate(f.context).recoil,"An editor-held key must not flip Toggle on resume");
    f.keys[65]=false;service.evaluate(f.context);f.keys[65]=true;
    require(service.evaluate(f.context).recoil,"A new real edge must still flip Toggle");
    f.context.inputCaptured=true;f.keys[65]=false;service.evaluate(f.context);
    f.context.inputCaptured=false;f.keys[65]=true;
    require(service.evaluate(f.context).recoil,"Capture must preserve Toggle without synthesizing another edge");
    bool captured=false;f.context.inputCapturedNow=[&]{return captured;};
    captured=true;f.keys[65]=false;
    require(!service.evaluate(f.context).recoil,"Live capture callback must block operational sinks");
    captured=false;f.keys[65]=true;
    require(service.evaluate(f.context).recoil,"Live capture callback must reseed edges while preserving Toggle");
}
void testIncompleteAndValidation()
{
    Service s;Fixture f;auto w=defaults();auto& g=w.graphs[0];g.links.erase(std::remove_if(g.links.begin(),g.links.end(),[](const Link& l){return l.toNode==6&&l.toPort==0;}),g.links.end());publish(s,w);require(!s.evaluate(f.context).aim,"Disconnected Enable must deactivate immediately");w=emptyWorkspace();publish(s,w);require(!s.evaluate(f.context).aim,"Missing sink must be inactive");
    std::string error;auto bad=defaults();node(bad.graphs[0],5).parameters["unknown"]=1;require(!validate(bad,error),"Unknown parameter must be rejected");bad=defaults();node(bad.graphs[0],5).parameters["kalman_warmup_frames"]=1.5;require(!validate(bad,error),"Fractional integer parameter must be rejected");bad=defaults();node(bad.graphs[0],5).parameters["lookahead"]=std::numeric_limits<double>::infinity();require(!validate(bad,error),"Infinite parameter must be rejected");
    bad=defaults();bad.graphs[0].nodes.push_back(defaultNode(NodeKind::AimOutput,90));require(!validate(bad,error),"Duplicate output must be rejected");bad.graphs[0].nodes.pop_back();bad.graphs[0].links.push_back({5,0,32,0});require(!validate(bad,error),"Cycle must be rejected");bad=defaults();bad.graphs[0].links.push_back({1,0,5,1});require(!validate(bad,error),"Mismatched pin types must be rejected");bad=defaults();bad.graphs[0].groups[0].nodes.push_back(99999);require(!validate(bad,error),"Dangling group member must be rejected");for(const auto& def:definitions())require(def.inputs.size()<=MaxPins,"Definition exceeds MaxPins");
}
void testMovementMetadataAndTiming()
{
    Service s;Fixture f;auto w=emptyWorkspace();auto& g=w.graphs[0];constant(g,1,1,true);g.nodes.push_back(defaultNode(NodeKind::Center,2));constant(g,3,1);auto delta=defaultNode(NodeKind::MouseDelta,4);delta.parameters["legacy_center"]=1;g.nodes.push_back(delta);g.nodes.push_back(defaultNode(NodeKind::NormalizeFps,5));g.nodes.push_back(defaultNode(NodeKind::CaptureFps,6));g.nodes.push_back(defaultNode(NodeKind::AimOutput,7));g.links={{1,0,7,0},{2,0,4,0},{3,0,4,1},{4,0,5,0},{6,0,5,1},{5,0,7,1}};
    f.context.captureFps=40;f.context.mouseDelta=[](Point,double,Point,const Node&){Movement m;m.steps={{2,0}};m.preciseCounts={2.99,0};m.hasPreciseCounts=true;m.pixelScaleX=2;m.pixelScaleY=3;return m;};publish(s,w);auto a=s.evaluate(f.context);require(a.aim&&a.movement.steps[0].dx==2,"FPS scaling must precede original truncation");require(a.movement.pixelScaleX==2&&!a.movement.hasPreciseCounts,"Calibration metadata must survive normalization");
    auto& r=w.graphs[2];constant(r,1,2);constant(r,2,0.25);r.nodes.push_back(defaultNode(NodeKind::Pulse,3));constant(r,4,5);r.nodes.push_back(defaultNode(NodeKind::RecoilOutput,5));r.links={{1,0,3,0},{2,0,3,1},{3,0,5,0},{4,0,5,1}};publish(s,w);f.context.elapsedSeconds=0.49;require(s.evaluate(f.context).recoil,"Pulse duty interval must be active");f.context.elapsedSeconds=0.5;require(!s.evaluate(f.context).recoil,"Pulse boundary must turn off");
}
struct VisionFixture
{
    Context context;
    FrameStream source;
    int captures=0,inferences=0,depths=0,masks=0,applies=0,maskFilters=0,collections=0,renders=0,screenshots=0;
    bool unsupportedDepth=false,failSecondary=false,renderedMask=false,renderedDepth=false;
    std::string collectedModel;
    std::vector<std::string> order;
    VisionFixture()
    {
        auto frame=std::make_shared<FrameData>();frame->storage=std::make_shared<const int>(7);frame->width=320;frame->height=320;frame->version=1;frame->generation=4;frame->timestampSeconds=1;source=frame;
        context.frame=source;context.center={160,160};
        context.captureFrame=[this](int,const Node&){++captures;order.push_back("capture");return source;};
        context.infer=[this](int,const FrameStream& frame,const Node& settings){
            ++inferences;order.push_back("infer");if(failSecondary&&settings.id==50)throw std::runtime_error("Secondary model unavailable");
            auto result=std::make_shared<DetectionFrame>();result->version=frame->version;result->generation=frame->generation;result->fresh=true;result->width=frame->width;result->height=frame->height;
            result->modelName=textParameter(settings,"model_path");result->objects={{10,10,20,20,0,0.9},{11,11,20,20,0,0.7},{100,100,10,10,1,0.05},{10,10,20,20,2,0.95}};
            return DetectionStream(result);
        };
        context.depth=[this](int,const FrameStream& frame,const Node&){++depths;order.push_back("depth");auto result=std::make_shared<FrameData>(*frame);result->storage=std::make_shared<const int>(11);if(unsupportedDepth){result->storage.reset();result->error="Depth backend unavailable";}return FrameStream(result);};
        context.depthToMask=[this](const FrameStream& frame,const Node&){++masks;order.push_back("mask");auto result=std::make_shared<MaskData>();result->storage=std::make_shared<const int>(13);result->width=frame->width;result->height=frame->height;result->version=frame->version;result->generation=frame->generation;return MaskStream(result);};
        context.applyMask=[this](const FrameStream& frame,const MaskStream&,const Node&){++applies;order.push_back("apply");auto result=std::make_shared<FrameData>(*frame);result->storage=std::make_shared<const int>(17);return FrameStream(result);};
        context.filterMask=[this](const DetectionStream& detections,const MaskStream&,const Node&){++maskFilters;order.push_back("filter-mask");auto result=std::make_shared<DetectionFrame>(*detections);if(result->objects.size()>1)result->objects.resize(1);return DetectionStream(result);};
        context.collect=[this](int,const FrameStream&,const DetectionStream& detections,const Node&){++collections;collectedModel=detections?detections->modelName:"";return true;};
        context.render=[this](int,const FrameStream&,const DetectionStream&,const MaskStream& mask,const FrameStream& depth,const Node&){++renders;renderedMask=static_cast<bool>(mask);renderedDepth=static_cast<bool>(depth);return true;};
        context.screenshot=[this](int,const FrameStream&,const Node&){++screenshots;return true;};
    }
};
const NodeValue& sampleNode(const Telemetry& telemetry,int graph,int id)
{
    for(const auto& entry:telemetry.graphs[graph])if(entry.nodeId==id)return entry;
    throw std::runtime_error("Missing node telemetry");
}
void testRealSignalTelemetryAndBriefEdges()
{
    Service service;Fixture fixture;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[2];
    auto key=defaultNode(NodeKind::KeyPressed,1);key.parameters["virtual_key"]=65;
    graph.nodes={key,defaultNode(NodeKind::RecoilOutput,3)};constant(graph,2,1);constant(graph,9,5);
    graph.links={{1,0,3,0},{2,0,3,1}};publish(service,workspace);service.evaluate(fixture.context);
    const auto first=service.telemetry();const auto firstSequence=sampleNode(first,2,1).signalSequence;
    require(firstSequence==1&&!sampleNode(first,2,1).active,"Initial false signal was marked active");
    require(sampleNode(first,2,9).signalSequence==0&&!sampleNode(first,2,9).value.valid,"An unused node emitted simulated signals");
    fixture.keys[65]=true;service.evaluate(fixture.context);service.evaluate(fixture.context);fixture.keys[65]=false;service.evaluate(fixture.context);
    std::this_thread::sleep_for(std::chrono::milliseconds(35));service.evaluate(fixture.context);
    const auto edges=service.telemetry();
    require(sampleNode(edges,2,1).signalSequence==firstSequence+2&&!sampleNode(edges,2,1).value.boolean,"A brief key pulse was lost between UI samples");
    require((sampleNode(edges,2,3).readInputs&3u)==3u,"The telemetry lost an input consumed during the short active branch");
    const auto sequence=sampleNode(edges,2,1).signalSequence;
    std::this_thread::sleep_for(std::chrono::milliseconds(35));service.evaluate(fixture.context);
    const auto unchanged=service.telemetry();
    require(sampleNode(unchanged,2,1).signalSequence==sequence&&sampleNode(unchanged,2,3).readInputs==1u,"Unchanged input generated another signal or consumed an inactive data branch");
}
void testFrameSignalsIgnoreRepaintsAndMonitoringCanSleep()
{
    Service service;VisionFixture fixture;std::chrono::steady_clock::time_point providerCompleted;
    const auto infer=fixture.context.infer;
    fixture.context.infer=[&](int key,const FrameStream& frame,const Node& node){const auto result=infer(key,frame,node);providerCompleted=std::chrono::steady_clock::now();return result;};
    service.evaluateVision(fixture.context);
    const auto first=service.telemetry();const auto sequence=sampleNode(first,3,1).signalSequence;
    require(first.graphSampledAt[3]>=providerCompleted&&sampleNode(first,3,2).signaledAt>=providerCompleted,"Provider telemetry was timestamped before its result existed");
    std::this_thread::sleep_for(std::chrono::milliseconds(35));service.evaluateVision(fixture.context);
    const auto same=service.telemetry();
    require(sampleNode(same,3,1).signalSequence==sequence&&sampleNode(same,3,2).signalSequence==sampleNode(first,3,2).signalSequence,"The same frame or a new result allocation generated fake data events");
    require(sampleNode(same,3,12).readInputs==0,"A disabled dataset branch reported reading frame data");
    auto next=std::make_shared<FrameData>(*fixture.source);next->version=2;next->timestampSeconds=2;fixture.source=next;
    std::this_thread::sleep_for(std::chrono::milliseconds(35));service.evaluateVision(fixture.context);
    const auto changed=service.telemetry();
    require(sampleNode(changed,3,1).signalSequence==sequence+1&&sampleNode(changed,3,2).signalSequence==sampleNode(same,3,2).signalSequence+1,"New frame identities did not produce real signals");
    auto layout=service.workspace();std::reverse(layout.graphs[3].nodes.begin(),layout.graphs[3].nodes.end());const auto revision=service.revision();publish(service,layout);
    std::this_thread::sleep_for(std::chrono::milliseconds(35));service.evaluateVision(fixture.context);
    const auto moved=service.telemetry();
    require(service.revision()==revision&&sampleNode(moved,3,1).signalSequence==sampleNode(changed,3,1).signalSequence,"Changing layout restarted the runtime signal counters");
    service.setTelemetryEnabled(false);const auto sampled=moved.graphSampledAt[3];const auto calls=fixture.captures;
    next=std::make_shared<FrameData>(*fixture.source);next->version=3;fixture.source=next;service.evaluateVision(fixture.context);
    require(fixture.captures==calls+1&&service.telemetry().graphSampledAt[3]==sampled,"Hidden-editor monitoring either kept copying telemetry or stopped real execution");
    service.setTelemetryEnabled(true);service.evaluateVision(fixture.context);
    const auto resumed=service.telemetry();
    require(resumed.graphSampledAt[3]>sampled&&sampleNode(resumed,3,1).value.frame->version==3,"Reopening the monitor did not immediately publish current data");
}
void testNewInputEventsPropagateThroughUnchangedValues()
{
    Service service;Fixture fixture;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[2];
    graph.nodes={defaultNode(NodeKind::Detections,1),defaultNode(NodeKind::TargetNearest,2),defaultNode(NodeKind::TargetPosition,3),
        defaultNode(NodeKind::PointX,4),defaultNode(NodeKind::RecoilOutput,5)};
    constant(graph,6,1,true);graph.links={{1,0,2,0},{2,0,3,0},{3,0,4,0},{6,0,5,0},{4,0,5,1}};
    publish(service,workspace);service.evaluate(fixture.context);const auto first=service.telemetry();
    auto next=std::make_shared<DetectionFrame>(*fixture.context.detections);next->version=2;fixture.context.detections=next;
    std::this_thread::sleep_for(std::chrono::milliseconds(35));service.evaluate(fixture.context);const auto second=service.telemetry();
    require(sampleNode(first,2,4).value.number==sampleNode(second,2,4).value.number,"Signal propagation fixture unexpectedly changed the coordinates");
    require(sampleNode(second,2,2).signalSequence==sampleNode(first,2,2).signalSequence+1&&
        sampleNode(second,2,4).signalSequence==sampleNode(first,2,4).signalSequence+1,"New frame data stopped propagating through an unchanged target or number");
}
void testVisionExecutionAndLaziness()
{
    Service service;Fixture controls;VisionFixture vision;
    service.evaluate(controls.context);require(vision.captures==0&&vision.inferences==0,"Control evaluation must not invoke Vision providers");
    auto action=service.evaluateVision(vision.context);
    require(action.visionDetections&&action.visionDetections->objects.size()==2,"Vision must execute confidence, limit and class-aware NMS on actual detections");
    require(vision.captures==1&&vision.inferences==1&&vision.depths==0&&vision.masks==0&&vision.applies==0&&vision.maskFilters==0,"Disabled mask chain must not invoke depth providers");
    require(vision.collections==0&&vision.renders==0,"Disabled auxiliary outputs must not execute callbacks");
    require(action.activeProviderKeys.size()==2&&action.activeDepthKeys.empty()&&action.activeMaskKeys.empty(),"Vision activity must identify actual providers only");
    Service wiredService;VisionFixture wiredFixture;auto wired=NodeTestData::previousStarters();auto& wiredVision=wired.graphs[3];
    auto gate=defaultNode(NodeKind::Boolean,50);gate.value=0;wiredVision.nodes.push_back(gate);wiredVision.links.push_back({50,0,12,0});
    publish(wiredService,wired);require(wiredService.providerNodes(NodeKind::DatasetOutput).size()==1,"Wired auxiliary enables must retain registered providers while the trigger is false");
    wiredService.evaluateVision(wiredFixture.context);require(wiredFixture.collections==0,"A false wired enable must keep its auxiliary output inactive");
    node(wiredVision,50).value=1;publish(wiredService,wired);wiredService.evaluateVision(wiredFixture.context);
    require(wiredFixture.collections==1,"Wired enable must override the detached enabled parameter");
    auto workspace=defaults();node(workspace.graphs[3],11).parameters["enabled"]=0;publish(service,workspace);
    service.evaluateVision(vision.context);require(vision.captures==1&&vision.inferences==1,"Disabled Vision output must skip all provider dependencies");
    require(service.providerNodes(NodeKind::CaptureSource).empty(),"Provider listing must exclude unreachable sources");
    workspace=defaults();node(workspace.graphs[3],7).parameters["limit"]=0;node(workspace.graphs[3],8).parameters["max_candidates"]=0;publish(service,workspace);
    require(service.evaluateVision(vision.context).visionDetections->objects.size()==2,"Zero detection and NMS candidate limits must preserve unlimited semantics");
}
void testVisionDepthOrderingAndOptionalOutputs()
{
    Service service;VisionFixture vision;auto workspace=defaults();auto& graph=workspace.graphs[3];
    for(int id:{3,4,5,10})node(graph,id).parameters["enabled"]=1;
    publish(service,workspace);auto action=service.evaluateVision(vision.context);
    const std::vector<std::string> expected={"capture","depth","mask","apply","infer","filter-mask"};
    require(vision.order==expected,"Masked-frame inference and detection mask must follow graph dependency order");
    require(action.visionDetections&&action.visionDetections->objects.size()==1,"Detection mask output must reach Vision output");
    require(action.activeDepthKeys==std::vector<int>{3000006}&&action.activeMaskKeys.size()==3,"Depth and mask lifecycle identities must remain separate");
}
void testVisionOptionalDepthFailure()
{
    Service service;VisionFixture vision;auto workspace=defaults();auto& graph=workspace.graphs[3];
    node(graph,13).parameters["enabled"]=1;settings(workspace,NodeKind::OverlayDepth,"depth_debug_overlay_enabled",1);node(graph,3).parameters["enabled"]=1;
    vision.unsupportedDepth=true;publish(service,workspace);auto action=service.evaluateVision(vision.context);
    require(action.visionDetections&&vision.renders==1&&!vision.renderedDepth,"Unavailable optional depth must not stop ordinary frame/detection rendering");
    const auto telemetry=service.telemetry();const auto found=std::find_if(telemetry.graphs[3].begin(),telemetry.graphs[3].end(),[](const NodeValue& value){return value.nodeId==3;});
    require(found!=telemetry.graphs[3].end()&&!found->value.valid&&found->value.error=="Depth backend unavailable","Unsupported provider must expose its node error");
}
void testMultipleInferenceAndCaptureConstraint()
{
    Service service;VisionFixture vision;auto workspace=defaults();auto& graph=workspace.graphs[3];
    node(graph,2).textParameters["model_path"]="primary.onnx";auto secondary=defaultNode(NodeKind::Inference,50);secondary.textParameters["model_path"]="secondary.onnx";graph.nodes.push_back(secondary);
    auto collect=defaultNode(NodeKind::CollectDataset,51);collect.parameters["enabled"]=1;graph.nodes.push_back(collect);
    graph.links.insert(graph.links.end(),{{1,0,50,0},{1,0,51,1},{50,0,51,2}});publish(service,workspace);auto action=service.evaluateVision(vision.context);
    require(vision.captures==1&&vision.inferences==2&&vision.collections==1,"A single captured frame must fan out to independent inference providers");
    require(action.visionDetections->modelName=="primary.onnx"&&vision.collectedModel=="secondary.onnx","Model provenance must follow each connected inference stream");
    require(service.providerNodes(NodeKind::Inference).size()==2,"Provider listing must include both reachable inference nodes");
    graph.nodes.push_back(defaultNode(NodeKind::CaptureSource,52));for(auto& link:graph.links)if(link.toNode==50&&link.toPort==0)link.fromNode=52;
    std::string error;require(!validate(workspace,error),"Two reachable physical capture sources must be rejected explicitly");
    node(graph,51).parameters["enabled"]=0;require(validate(workspace,error),"An unreachable alternative capture source must remain editable");
    publish(service,workspace);require(service.providerNodes(NodeKind::CaptureSource).size()==1,"Unreachable captures must not create devices");
}
void testVisionProviderFailureIsolation()
{
    Service service;VisionFixture vision;auto workspace=defaults();auto& graph=workspace.graphs[3];
    graph.nodes.push_back(defaultNode(NodeKind::Inference,50));auto collect=defaultNode(NodeKind::CollectDataset,51);collect.parameters["enabled"]=1;graph.nodes.push_back(collect);
    graph.links.insert(graph.links.end(),{{1,0,50,0},{1,0,51,1},{50,0,51,2}});vision.failSecondary=true;publish(service,workspace);
    const auto action=service.evaluateVision(vision.context);require(action.visionDetections&&vision.collections==0,"A failing secondary provider must not disable the primary Vision output");
    const auto values=service.telemetry().graphs[3];const auto failed=std::find_if(values.begin(),values.end(),[](const NodeValue& value){return value.nodeId==50;});
    require(failed!=values.end()&&failed->value.error=="Secondary model unavailable","Provider exception must appear on its node");
}
void testConcurrentVisionAndControl()
{
    Service service;Fixture control;VisionFixture vision;std::atomic<bool> okay{true};
    std::thread mouse([&]{for(int i=0;i<200;++i)if(!service.evaluate(control.context).aim)okay=false;});
    std::thread detector([&]{for(int i=0;i<200;++i)if(!service.evaluateVision(vision.context).visionDetections)okay=false;});
    mouse.join();detector.join();require(okay,"Concurrent control and Vision contexts must retain independent mutable caches");
    const auto telemetry=service.telemetry();require(!telemetry.graphs[0].empty()&&!telemetry.graphs[3].empty(),"Concurrent telemetry publication must preserve both domains");
    require(telemetry.graphSampledAt[0]!=std::chrono::steady_clock::time_point{}&&telemetry.graphSampledAt[3]!=std::chrono::steady_clock::time_point{},"Graph freshness must be recorded separately");
}
void testScreenshotOutput()
{
    Service service;VisionFixture vision;auto workspace=NodeTestData::previousStarters();auto& graph=workspace.graphs[3];
    node(graph,11).parameters["enabled"]=0;
    bool held=false;int queries=0;
    vision.context.keyDown=[&](int key){++queries;return key==65&&held;};
    publish(service,workspace);auto action=service.evaluateVision(vision.context);
    require(vision.captures==0&&vision.screenshots==0&&queries==0,"Unassigned screenshot key must not query input or acquire a frame");
    node(graph,15).parameters["virtual_key"]=65;publish(service,workspace);service.evaluateVision(vision.context);
    require(vision.captures==0&&vision.screenshots==0,"False screenshot trigger must skip frame acquisition");
    held=true;action=service.evaluateVision(vision.context);
    require(vision.captures==1&&vision.screenshots==1&&vision.inferences==0,"Screenshot output must consume its frame independently of inference");
    require(action.activeScreenshotKeys==std::vector<int>{3000017},"Screenshot activity must identify the invoked output");
    held=false;action=service.evaluateVision(vision.context);
    require(vision.screenshots==1&&action.activeScreenshotKeys.empty(),"Released screenshot trigger must not invoke its output");
    node(graph,14).bypassed=true;publish(service,workspace);held=true;service.evaluateVision(vision.context);
    require(vision.screenshots==1,"Bypassed screenshot output must remain inactive");
}
std::string oldFormat(const Workspace& w,int version)
{
    std::ostringstream out;out<<std::setprecision(17)<<"SUNONE_NODE_GRAPH "<<version<<'\n';
    for(std::size_t graph=0;graph<(version>=4?GraphCount:3);++graph)
    {
        const auto& g=w.graphs[graph];out<<"GRAPH "<<static_cast<int>(g.kind)<<" 40 40 1 "<<g.nodes.size()<<' '<<g.links.size();if(version>=2)out<<" 0";if(version>=5)out<<" 0";out<<'\n';
        for(const auto& n:g.nodes)
        {
            out<<"NODE "<<n.id<<' '<<static_cast<int>(n.kind)<<' '<<n.x<<' '<<n.y<<' '<<n.value;
            if(version>=2)out<<' '<<n.bypassed<<' '<<n.collapsed;
            if(version>=3)out<<' '<<n.parameters.size()<<' '<<n.textParameters.size();if(version>=6)out<<' '<<n.disabled;if(version>=7)out<<' '<<n.width<<' '<<n.height;if(version==9)out<<" 0";out<<'\n';
            if(version>=3)
            {
                for(const auto& p:n.parameters)out<<"PARAM "<<std::quoted(p.first)<<' '<<p.second<<'\n';
                for(const auto& p:n.textParameters)out<<"TEXT "<<std::quoted(p.first)<<' '<<std::quoted(p.second)<<'\n';
            }
        }
        for(const auto& l:g.links){out<<"LINK "<<l.fromNode<<' '<<l.fromPort<<' '<<l.toNode<<' '<<l.toPort;if(version>=5)out<<" 0";out<<'\n';}
    }
    out<<"END\n";return out.str();
}
void testPersistence()
{
    auto w=defaults();const int profiles=0;int calibration=0;for(const auto& n:w.graphs[0].nodes)if(n.kind==NodeKind::MouseProfile)calibration=n.id;int time=0;for(const auto& n:w.graphs[0].nodes)if(n.kind==NodeKind::PredictionTime)time=n.id;node(w.graphs[0],time).parameters["lookahead"]=0.123456789;node(w.graphs[profiles],calibration).textParameters["profile_name"]="Custom \"precise\" profile";node(w.graphs[0],5).collapsed=true;node(w.graphs[0],11).bypassed=true;std::string text,error;Workspace restored;
    require(serialize(w,text,error)&&text.find("SUNONE_NODE_GRAPH "+std::to_string(CurrentFormatVersion))==0&&deserialize(text,restored,error),"Current parameters did not roundtrip");require(parameter(node(restored.graphs[0],time),"lookahead")==0.123456789&&textParameter(node(restored.graphs[profiles],calibration),"profile_name")=="Custom \"precise\" profile","Export lost algorithm parameters");require(node(restored.graphs[0],5).collapsed&&node(restored.graphs[0],11).bypassed,"UI flags lost during export");require(restored.sourceVersion==CurrentFormatVersion&&restored.graphs[0].groups[0].nodes==w.graphs[0].groups[0].nodes,"Workspace metadata lost");
    for(int version:{1,2}){require(deserialize(oldFormat(w,version),restored,error),"Legacy workspace must remain readable");require(restored.sourceVersion==version&&node(restored.graphs[0],5).parameters.empty(),"Legacy graph must remain identifiable for migration");}require(!deserialize(text.substr(0,text.size()/2),restored,error),"Truncation must be rejected");require(!deserialize(text+"UNEXPECTED",restored,error),"Trailing data must be rejected");
    const auto version3=oldFormat(w,3);
    require(deserialize(version3,restored,error)&&restored.sourceVersion==3&&restored.graphs[3].kind==GraphKind::Vision,"Version 3 must gain a Vision graph without losing control settings");
    require(parameter(node(restored.graphs[0],time),"lookahead")==0.123456789,"Version 3 import must preserve node-owned tuning");
    require(deserialize(oldFormat(w,4),restored,error)&&restored.sourceVersion==4&&restored.graphs[3].ties.empty(),"Version 4 files must remain readable without wire ties");
    for(int version:{5,6,7,8,9,10})require(deserialize(oldFormat(w,version),restored,error)&&restored.sourceVersion==version&&
        node(restored.graphs[0],5).width==0&&node(restored.graphs[0],5).height==0,"Older workspaces did not load with automatic dimensions");
    require(!deserialize(std::string(MaxWorkspaceBytes+1,'x'),restored,error),"Oversized workspace must be rejected before parsing");
    const auto path=std::filesystem::temp_directory_path()/("sunone_nodes_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".ini");struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code e;std::filesystem::remove(path,e);std::filesystem::remove(path.string()+".tmp",e);}}cleanup{path};Service s;publish(s,w);require(s.save(path.string(),error),"File save failed");w.graphs[0].panX=123;publish(s,w);require(s.save(path.string(),error),"Atomic replacement failed");Service loaded;require(loaded.load(path.string(),error)&&loaded.workspace().graphs[0].panX==123,"File roundtrip failed");{std::ofstream file(path);file<<"corrupt";}require(!loaded.load(path.string(),error)&&loaded.workspace().graphs[0].panX==123,"Corrupt import must preserve current graph");
}
void testIntegerArithmeticAndConversions()
{
    require(canConnectTypes(ValueType::Integer,ValueType::Float)&&!canConnectTypes(ValueType::Float,ValueType::Integer),"Numeric widening or explicit narrowing rules are incorrect");
    const auto run=[](NodeKind kind,std::int32_t a,std::int32_t b,std::int32_t c=0){
        Service service;Fixture fixture;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[2];
        for(int id=1;id<=3;++id){auto n=defaultNode(NodeKind::Integer,id);n.value=id==1?a:id==2?b:c;graph.nodes.push_back(n);}
        graph.nodes.push_back(defaultNode(kind,4));graph.nodes.push_back(defaultNode(NodeKind::Boolean,5));graph.nodes.push_back(defaultNode(NodeKind::RecoilOutput,6));
        for(std::size_t pin=0;pin<definition(kind).inputs.size();++pin)graph.links.push_back({static_cast<int>(pin)+1,0,4,static_cast<int>(pin)});
        graph.links.push_back({5,0,6,0});graph.links.push_back({4,0,6,1});publish(service,workspace);
        const auto actions=service.evaluate(fixture.context);const auto value=sampleNode(service.telemetry(),2,4).value;
        std::string text,error;Workspace loaded;require(serialize(workspace,text,error)&&deserialize(text,loaded,error),"Typed numeric graph failed to roundtrip");
        require(node(loaded.graphs[2],1).kind==NodeKind::Integer&&node(loaded.graphs[2],1).value==a,"Int constant changed during import/export");
        return std::pair{actions,value};
    };
    for(const auto example:std::vector<std::tuple<NodeKind,int,int,int,int>>{
        {NodeKind::IntegerAdd,7,-3,0,4},{NodeKind::IntegerSubtract,7,-3,0,10},
        {NodeKind::IntegerMultiply,-7,3,0,-21},{NodeKind::IntegerDivide,-7,3,0,-2},{NodeKind::IntegerClamp,20,9,-4,9}})
    {
        const auto result=run(std::get<0>(example),std::get<1>(example),std::get<2>(example),std::get<3>(example));
        require(result.first.recoil&&result.second.type==ValueType::Integer&&result.second.integer==std::get<4>(example)&&result.first.recoilStrength==std::get<4>(example),"Int arithmetic or widening to a Float input lost its value");
    }
    for(const auto example:std::vector<std::tuple<NodeKind,int,int>>{
        {NodeKind::IntegerAdd,MaxInteger,1},{NodeKind::IntegerSubtract,MinInteger,1},
        {NodeKind::IntegerMultiply,MaxInteger,2},{NodeKind::IntegerDivide,MinInteger,-1},{NodeKind::IntegerDivide,10,0}})
    {
        const auto result=run(std::get<0>(example),std::get<1>(example),std::get<2>(example));
        require(!result.first.recoil&&!result.second.valid&&!result.second.error.empty(),"Int overflow or division by zero reached an output");
    }
    for(int boundary:{MinInteger,MaxInteger}){const auto result=run(NodeKind::ToFloat,boundary,0);require(result.second.type==ValueType::Float&&result.second.number==boundary,"Int to Float lost boundary precision");}
    for(int mode=0;mode<4;++mode)
    {
        Service service;Fixture fixture;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[2];
        constant(graph,1,-3.75);graph.nodes.push_back(defaultNode(NodeKind::ToInteger,2));node(graph,2).parameters["rounding"]=mode;
        graph.nodes.push_back(defaultNode(NodeKind::RerouteInteger,3));graph.nodes.push_back(defaultNode(NodeKind::ToFloat,4));constant(graph,5,1,true);graph.nodes.push_back(defaultNode(NodeKind::RecoilOutput,6));
        graph.links={{1,0,2,0},{2,0,3,0},{3,0,4,0},{5,0,6,0},{4,0,6,1}};publish(service,workspace);
        const int expected=mode==1||mode==3?-4:-3;const auto actions=service.evaluate(fixture.context);const auto telemetry=service.telemetry();
        require(actions.recoil&&actions.recoilStrength==expected&&sampleNode(telemetry,2,3).value.type==ValueType::Integer&&sampleNode(telemetry,2,3).value.integer==expected,"Explicit rounding or Int reroute changed type/value");
        require(sampleNode(telemetry,2,4).value.type==ValueType::Float,"Explicit Int to Float did not produce Float");
        std::string error;require(!canConnect(graph,{1,0,3,0},error),"Float connected directly to an Int socket");
    }
    Service service;Fixture fixture;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[2];
    constant(graph,1,1e9);graph.nodes.push_back(defaultNode(NodeKind::Multiply,2));graph.nodes.push_back(defaultNode(NodeKind::ToInteger,3));constant(graph,4,1,true);graph.nodes.push_back(defaultNode(NodeKind::RecoilOutput,5));
    graph.links={{1,0,2,0},{1,0,2,1},{2,0,3,0},{4,0,5,0},{3,0,5,1}};publish(service,workspace);
    require(!service.evaluate(fixture.context).recoil&&!sampleNode(service.telemetry(),2,3).value.error.empty(),"Out-of-range Float to Int wrapped or clamped silently");
    std::string error;graph.nodes.push_back(defaultNode(NodeKind::Integer,9));node(graph,9).value=1.5;require(!validate(workspace,error),"Fractional Int constant was accepted");
    node(graph,9).value=static_cast<double>(MaxInteger)+1;require(!validate(workspace,error),"Out-of-range Int constant was accepted");
}
void testIntegerTargetPropertiesKeepLegacyConnections()
{
    Service service;Fixture fixture;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[2];
    for(const auto entry:std::vector<std::pair<NodeKind,int>>{{NodeKind::Detections,1},{NodeKind::TargetNearest,2},{NodeKind::TargetClass,3},
        {NodeKind::TargetTrackId,4},{NodeKind::TargetMissedFrames,5},{NodeKind::Add,6},{NodeKind::Add,7},{NodeKind::Boolean,8},{NodeKind::RecoilOutput,9}})
        graph.nodes.push_back(defaultNode(entry.first,entry.second));
    graph.links={{1,0,2,0},{2,0,3,0},{2,0,4,0},{2,0,5,0},{3,0,6,0},{4,0,6,1},{6,0,7,0},{5,0,7,1},{8,0,9,0},{7,0,9,1}};
    fixture.context.selectTarget=[](int,const DetectionStream&,bool,const Node&){TargetData target;target.valid=true;target.classId=7;target.trackId=19;target.missedFrames=2;return target;};
    std::string error;Workspace loaded;require(deserialize(oldFormat(workspace,7),loaded,error),"Version 7 graph could not retain numeric property connections");publish(service,loaded);
    require(service.evaluate(fixture.context).recoilStrength==28,"Legacy Int property to Float math changed its result");
    for(int id:{3,4,5})require(sampleNode(service.telemetry(),2,id).value.type==ValueType::Integer,"A target identifier or frame count is still Float");
    node(loaded.graphs[2],6).bypassed=true;publish(service,loaded);require(service.evaluate(fixture.context).recoilStrength==9,"Bypass lost integer widening");
    require(sampleNode(service.telemetry(),2,6).value.type==ValueType::Float,"Bypassed Float node returned Int despite its declared output");
}
void testWireTiesArePersistentVisualMetadata()
{
    Service service;Fixture fixture;auto workspace=defaults();const auto original=service.evaluate(fixture.context);const auto revision=service.revision();
    auto& graph=workspace.graphs[0];for(auto& link:graph.links)link.ties.clear();graph.ties={{7,352,192,37.5f},{9,640,160,-15}};graph.links[0].ties={7,9};graph.links[1].ties={7};
    publish(service,workspace);const auto tied=service.evaluate(fixture.context);
    require(service.revision()==revision&&tied.aim==original.aim&&tied.aimPoint.x==original.aimPoint.x&&tied.aimPoint.y==original.aimPoint.y,"Wire ties changed graph execution");
    std::string text,error;Workspace restored;require(serialize(workspace,text,error)&&deserialize(text,restored,error),"Wire tie export/import failed");
    require(restored.graphs[0].ties.size()==2&&restored.graphs[0].ties[0].angle==37.5f&&restored.graphs[0].ties[0].x==352&&restored.graphs[0].links[0].ties==std::vector<int>({7,9}),"Wire tie positions, rotation or route order were lost");
    auto invalid=workspace;invalid.graphs[0].links[0].ties={7,7};require(!validate(invalid,error),"Repeated tie waypoints must be rejected");
    invalid=workspace;invalid.graphs[0].links[0].ties={99};require(!validate(invalid,error),"Missing tie references must be rejected");
    invalid=workspace;invalid.graphs[0].ties[0].angle=std::numeric_limits<float>::infinity();require(!validate(invalid,error),"Invalid tie rotation must be rejected");
    graph.ties[0].x+=32;graph.ties[0].angle=-90;publish(service,workspace);require(service.revision()==revision,"Moving a tie reset running graph state");
}
void testWireBendsArePersistentVisualMetadata()
{
    Service service;auto workspace=defaults();publish(service,workspace);const auto revision=service.revision();
    workspace.graphs[0].links[0].bend={0.25f,-0.75f};workspace.graphs[0].links[1].bend={1,1};
    publish(service,workspace);require(service.revision()==revision,"Changing wire curvature restarted graph execution");
    std::string text,error;Workspace restored;
    require(serialize(workspace,text,error)&&deserialize(text,restored,error),"Wire bend export/import failed");
    require(restored.graphs[0].links[0].bend.roundness==0.25f&&restored.graphs[0].links[0].bend.twist==-0.75f&&
        restored.graphs[0].links[1].bend.roundness==1&&restored.graphs[0].links[1].bend.twist==1,"Wire bends lost precision or ordering on export/import");
    for(const auto bend:{WireBend{-0.01f,0},WireBend{1.01f,0},WireBend{0.5f,-1.01f},WireBend{0.5f,1.01f},
        WireBend{std::numeric_limits<float>::infinity(),0},WireBend{0.5f,std::numeric_limits<float>::quiet_NaN()}})
    {
        auto invalid=workspace;invalid.graphs[0].links[0].bend=bend;
        require(!validate(invalid,error),"An out-of-range or non-finite wire bend was accepted");
    }
    // Version 12 already has all node settings but no wire curvature fields.
    auto version12=workspace;version12.graphs.resize(GraphCount);require(serialize(version12,text,error),"Cannot build the four-workflow legacy fixture");
    std::istringstream input(text);std::ostringstream legacy;std::string line;
    while(std::getline(input,line))
    {
        if(line.rfind("PAGES ",0)==0||line.rfind("PAGE ",0)==0)continue;
        if(line.rfind("SUNONE_NODE_GRAPH ",0)==0)line="SUNONE_NODE_GRAPH 12";
        else if(line.rfind("LINK ",0)==0)
        {
            std::istringstream fields(line);std::string token;int from=0,out=0,to=0,in=0;std::size_t ties=0;
            fields>>token>>from>>out>>to>>in>>ties;std::ostringstream link;
            link<<token<<' '<<from<<' '<<out<<' '<<to<<' '<<in<<' '<<ties;
            for(std::size_t i=0;i<ties;++i){int id=0;fields>>id;link<<' '<<id;}line=link.str();
        }
        legacy<<line<<'\n';
    }
    require(deserialize(legacy.str(),restored,error),"Version 12 workflows no longer load");
    for(const auto& graph:restored.graphs)for(const auto& link:graph.links)
        require(link.bend.roundness==WireBend{}.roundness&&link.bend.twist==0,"Old workflows received a non-neutral manual bend");
}
void testLayoutFitsExpandedNodesAndNestedGroups()
{
    auto workspace=defaults();
    const auto check=[](const Graph& graph)
    {
        for(std::size_t a=0;a<graph.nodes.size();++a)
        {
            const auto& first=graph.nodes[a];
            const float anchorX=first.x+(Layout::isReroute(first.kind)?32:0),anchorY=first.y+(Layout::isReroute(first.kind)?16:0);
            require(anchorX==Layout::snap(anchorX)&&anchorY==Layout::snap(anchorY),"Layout left a node's visible anchor off the grid");
            for(std::size_t b=a+1;b<graph.nodes.size();++b)
            {
                const auto& second=graph.nodes[b];
                require(first.x+Layout::width(first)+32<=second.x||second.x+Layout::width(second)+32<=first.x||
                    first.y+Layout::height(first,true)+32<=second.y||second.y+Layout::height(second,true)+32<=first.y,"Expanded cards collide or lack space after arrangement");
            }
        }
        for(const auto& group:graph.groups)for(int id:group.nodes)
        {
            const auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const Node& member){return member.id==id;});
            require(found!=graph.nodes.end(),"Group member disappeared");const auto& member=*found;
            require(member.x>=group.x+32&&member.y>=group.y+64&&member.x+Layout::width(member)<=group.x+group.width-32&&
                member.y+Layout::height(member,true)<=group.y+group.height-32,"An expanded card does not fit its group");
        }
    };
    for(auto& graph:workspace.graphs){Layout::arrange(graph);check(graph);require(!Layout::arrange(graph),"Arrangement is not deterministic");}
    Graph nested;nested.kind=GraphKind::Aim;
    nested.nodes={defaultNode(NodeKind::Number,1),defaultNode(NodeKind::Add,2),defaultNode(NodeKind::Number,3),
        defaultNode(NodeKind::RerouteNumber,4),defaultNode(NodeKind::Clamp,5)};
    nested.nodes[0].collapsed=true;nested.nodes[0].value=12.75;nested.nodes[1].bypassed=true;
    nested.links={{1,0,2,0},{3,0,2,1},{2,0,4,0},{4,0,5,0}};
    nested.groups={{1,"Parent",0,0,256,192,0xffcccccc,{1,2,3,4,5}},
        {3,"Routing",0,0,256,192,0xffcccccc,{4,5}},{2,"Sources",0,0,256,192,0xffcccccc,{1,2,3}}};
    nested.ties={{1,0,0,30}};nested.links[2].ties={1};
    require(Layout::arrange(nested),"Overlapping nested graph was not arranged");check(nested);
    require(node(nested,2).x<node(nested,4).x,"Group creation order overrode upstream/downstream dependencies");
    require(nested.nodes[0].collapsed&&nested.nodes[0].value==12.75&&nested.nodes[1].bypassed&&nested.links[2].ties==std::vector<int>{1}&&nested.ties[0].angle==30,
        "Layout changed node state or tie membership");
    for(int g:{1,2})require(nested.groups[g].x>=nested.groups[0].x+32&&nested.groups[g].y>=nested.groups[0].y+64&&
        nested.groups[g].x+nested.groups[g].width<=nested.groups[0].x+nested.groups[0].width-32&&
        nested.groups[g].y+nested.groups[g].height<=nested.groups[0].y+nested.groups[0].height-32,"Nested frame does not fit its parent");
    require(!Layout::arrange(nested),"Arranging nested groups twice shifted nodes or ties");
    Graph large;large.kind=GraphKind::Aim;
    for(int i=0;i<static_cast<int>(MaxNodes);++i)large.nodes.push_back(defaultNode(i<8?NodeKind::Number:NodeKind::Clamp,i+1));
    for(int i=8;i<static_cast<int>(MaxNodes)&&large.links.size()<MaxLinks;++i)
        for(int pin=0;pin<3&&large.links.size()<MaxLinks;++pin)large.links.push_back({1+((i*7+pin*11)%i),0,i+1,pin});
    require(Layout::arrange(large),"Maximum graph was not arranged");check(large);
    require(!Layout::arrange(large),"Maximum graph arrangement is not stable");
}
void testHistoricalStarterLayoutMigrationPreservesWiring()
{
    auto workspace=NodeTestData::previousStarters();auto& legacy=workspace.graphs[2];
    legacy.nodes.erase(std::remove_if(legacy.nodes.begin(),legacy.nodes.end(),[](const Node& node){return node.id>7;}),legacy.nodes.end());
    legacy.links.erase(std::remove_if(legacy.links.begin(),legacy.links.end(),[](const Link& link){return link.fromNode>7;}),legacy.links.end());
    for(auto& entry:legacy.nodes)
    {
        if(entry.id<=4){entry.x=(entry.id-1)*256.0f;entry.y=0;}
        else{entry.x=(entry.id-5)*256.0f;entry.y=384;}
    }
    legacy.groups={{1,"Activation",-32,-128,1024,384,0x40ffcd60u,{1,2,3,4}},
        {2,"Recoil amount and output",-32,320,768,384,0x40adde66u,{5,6,7}}};
    node(legacy,6).parameters["strength"]=17.25;node(legacy,3).collapsed=true;
    legacy.links.erase(legacy.links.begin());const auto links=legacy.links;
    Service service;publish(service,workspace);const auto revision=service.revision();
    require(migrateStarterLayout(legacy),"Historical starter geometry was not recognized");
    publish(service,workspace);require(service.revision()==revision,"Layout migration reset execution");
    require(parameter(node(legacy,6),"strength")==17.25&&node(legacy,3).collapsed&&legacy.links.size()==links.size(),"Layout migration replaced settings, node state or wiring");
    for(std::size_t i=0;i<links.size();++i)require(legacy.links[i].fromNode==links[i].fromNode&&legacy.links[i].toNode==links[i].toNode&&
        legacy.links[i].fromPort==links[i].fromPort&&legacy.links[i].toPort==links[i].toPort,"Layout migration rewired the graph");
    require(!migrateStarterLayout(legacy),"Starter migration ran more than once");
    legacy.groups[0].title="Custom stage";legacy.nodes[0].x+=64;
    require(!migrateStarterLayout(legacy),"Loading overwrote a custom layout");
}
void testSplitStarterWorkflowMigratesWithoutChangingLogic()
{
    auto workspace=NodeTestData::previousStarters();auto& graph=workspace.graphs[3];
    std::vector<int> outputs{11,12,13};
    for(const auto& entry:graph.nodes)if((entry.kind>=NodeKind::OverlayBoxes&&entry.kind<=NodeKind::OverlayCorrection)||entry.kind==NodeKind::DatasetLabels)outputs.push_back(entry.id);
    graph.groups={{1,"01 Capture and depth",0,0,256,192,0x40ffcd60u,{1,3,4}},
        {2,"02 Model inference",0,0,256,192,0x40ff93adu,{5,2}},
        {3,"03 Detection filters",0,0,256,192,0x40adde66u,{6,7,8,9,10}},
        {4,"04 Vision and visual outputs",0,0,256,192,0x40ffcd60u,outputs},
        {5,"05 Screenshots",0,0,256,192,0x40ff93adu,{15,14}}};
    Layout::arrange(graph,6);
    node(graph,1).parameters["capture_fps"]=237;node(graph,13).collapsed=true;
    const auto old=graph;
    Service service;publish(service,workspace);const auto revision=service.revision();
    require(migrateStarterLayout(graph),"The previously shipped split workflow was not recognized");
    publish(service,workspace);
    require(service.revision()==revision&&graph.nodes.size()==old.nodes.size()&&graph.links.size()==old.links.size()&&
        parameter(node(graph,1),"capture_fps")==237&&node(graph,13).collapsed,"Workflow migration changed runtime logic, settings or node state");
    for(std::size_t i=0;i<old.links.size();++i)require(std::tie(graph.links[i].fromNode,graph.links[i].fromPort,graph.links[i].toNode,graph.links[i].toPort)==
        std::tie(old.links[i].fromNode,old.links[i].fromPort,old.links[i].toNode,old.links[i].toPort),"Workflow migration changed a wire");
    require(graph.groups.size()==6,"The default output branches were not separated into their stages");
    const auto owner=[&](int id){for(const auto& group:graph.groups)if(std::find(group.nodes.begin(),group.nodes.end(),id)!=group.nodes.end())return group.id;return 0;};
    require(owner(11)==owner(10)&&owner(12)!=owner(13)&&owner(14)!=owner(13),"Vision, dataset, overlay and screenshot outputs are still mixed together");
    float oldRight=0,newRight=0;
    for(const auto& group:old.groups)oldRight=std::max(oldRight,group.x+group.width);
    for(const auto& group:graph.groups){newRight=std::max(newRight,group.x+group.width);require(group.width<=1408,"A default stage still stretches across too many columns");}
    require(newRight<oldRight*0.8f,"The default workflow did not become more compact");
    require(!Layout::arrange(graph)&&!migrateStarterLayout(graph),"The new default workflow changes on every arrangement/load");
    graph=old;node(graph,1).x+=32;require(!migrateStarterLayout(graph),"A manually moved node was mistaken for shipped geometry");
}
void testManualDimensionsAreDurablePresentationOnly()
{
    auto workspace=defaults();Service service;publish(service,workspace);const auto revision=service.revision();
    auto& sized=node(workspace.graphs[0],5);sized.width=416;sized.height=640;sized.collapsed=true;
    publish(service,workspace);require(service.revision()==revision,"Resizing restarted execution");
    require(Layout::width(sized)==416&&Layout::height(sized)<640&&Layout::height(sized,true)==640,"Collapsed presentation lost the expanded manual size");
    std::string text,error;Workspace loaded;require(serialize(workspace,text,error)&&deserialize(text,loaded,error),"Manual size did not export/import");
    const auto& restored=node(loaded.graphs[0],5);require(restored.width==416&&restored.height==640&&restored.collapsed,"Reopening a collapsed node lost its size");
    require(!migrateStarterLayout(loaded.graphs[0]),"Starter migration overrode a manual size");
    Layout::arrange(loaded.graphs[0]);require(node(loaded.graphs[0],5).width==416&&node(loaded.graphs[0],5).height==640,"Automatic arrangement replaced a manual size");
    auto invalid=workspace;node(invalid.graphs[0],5).width=-1;require(!validate(invalid,error),"Negative node width was accepted");
    invalid=workspace;node(invalid.graphs[0],5).width=Layout::MaxWidth+1;require(!validate(invalid,error),"Excessive node width was accepted");
    invalid=workspace;node(invalid.graphs[0],5).height=32;require(!validate(invalid,error),"A manual height covered its ports and fields");
    invalid=workspace;node(invalid.graphs[0],5).height=std::numeric_limits<float>::infinity();require(!validate(invalid,error),"Non-finite node height was accepted");
    invalid=workspace;invalid.graphs[0].nodes.push_back(defaultNode(NodeKind::RerouteNumber,99));node(invalid.graphs[0],99).width=320;
    require(!validate(invalid,error),"A reroute dot accepted card dimensions");
}
void testDisableIsDistinctFromBypassAndPersists()
{
    Service service;Fixture fixture;auto workspace=defaults();auto& graph=workspace.graphs[0];
    node(graph,5).disabled=true;publish(service,workspace);const auto disabled=service.evaluate(fixture.context);
    require(!disabled.aim&&disabled.activePredictionKeys.empty(),"Disabled processing nodes must stop their branch and callbacks");
    node(graph,5).disabled=false;node(graph,5).bypassed=true;publish(service,workspace);
    require(service.evaluate(fixture.context).aim,"Bypass must pass the input through instead of disabling its branch");
    node(graph,6).disabled=true;publish(service,workspace);require(!service.evaluate(fixture.context).aim,"A disabled output still performed its action");
    std::string text,error;Workspace restored;require(serialize(workspace,text,error)&&deserialize(text,restored,error)&&node(restored.graphs[0],6).disabled,"Disabled state was lost on export/import");
    const std::string version5="SUNONE_NODE_GRAPH 5\nGRAPH 0 40 40 1 2 1 0 1\nNODE 1 12 0 0 1 0 0 0 0\nNODE 2 16 64 0 0 0 0 0 0\nLINK 1 0 2 0 1 1\nTIE 1 32 0 15\nGRAPH 1 40 40 1 0 0 0 0\nGRAPH 2 40 40 1 0 0 0 0\nGRAPH 3 40 40 1 0 0 0 0\nEND\n";
    require(deserialize(version5,restored,error)&&!restored.graphs[0].nodes[0].disabled&&restored.graphs[0].links[0].ties==std::vector<int>{1},"Version 5 routes must load without disabled flags");
}
}
#include "node_graph_binding_tests.h"
#include "node_output_layers_tests.h"
#include "node_workflow_tests.h"
#include "node_migration_audit_tests.h"
#include "node_shared_calibration_tests.h"
#include "node_runtime_audit_tests.h"
#include "node_reroute_delete_tests.h"
#include "node_universal_preview_tests.h"
#include "node_starter_layout_tests.h"
int main()
{
    const auto run=[](const char* name,auto test){try{test();}catch(const std::exception& error){throw std::runtime_error(std::string(name)+": "+error.what());}};
    try{run("testStarterLayoutsAreExactAndPortable",testStarterLayoutsAreExactAndPortable);}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
    try{run("testUniversalPreviewAcceptsValuesInEveryWorkflow",testUniversalPreviewAcceptsValuesInEveryWorkflow);run("testUniversalPreviewNumbersStructuresAndImageCompatibility",testUniversalPreviewNumbersStructuresAndImageCompatibility);}
    catch(const std::exception& error){std::cerr<<"Universal preview test failed: "<<error.what()<<'\n';return 1;}
    try{run("testRerouteDeletionPreservesBranchesAndRoutes",testRerouteDeletionPreservesBranchesAndRoutes);run("testRerouteDeletionSupportsEveryTypeAndRouteLimits",testRerouteDeletionSupportsEveryTypeAndRouteLimits);}
    catch(const std::exception& error){std::cerr<<"Reroute deletion test failed: "<<error.what()<<'\n';return 1;}
    try{run("testEventStateSurvivesUnrelatedLogicEdits",testEventStateSurvivesUnrelatedLogicEdits);run("testUnavailableTriggersDoNotCreateRecoveryEdges",testUnavailableTriggersDoNotCreateRecoveryEdges);run("testErrorCausesPropagateAndRecover",testErrorCausesPropagateAndRecover);run("testDormantBranchesDoNotReplayKeyEdges",testDormantBranchesDoNotReplayKeyEdges);}
    catch(const std::exception& error){std::cerr<<"Node runtime audit failed: "<<error.what()<<'\n';return 1;}
    try{run("testSharedCalibrationAcrossWorkflows",testSharedCalibrationAcrossWorkflows);run("testProcessingMigrationPreservesScopeAndFullGraphs",testProcessingMigrationPreservesScopeAndFullGraphs);run("testRecoilCalibrationAndMigration",testRecoilCalibrationAndMigration);}
    catch(const std::exception& error){std::cerr<<"Recoil calibration test failed: "<<error.what()<<'\n';return 1;}
    try{run("testLayoutFitsExpandedNodesAndNestedGroups",testLayoutFitsExpandedNodesAndNestedGroups);run("testHistoricalStarterLayoutMigrationPreservesWiring",testHistoricalStarterLayoutMigrationPreservesWiring);run("testSplitStarterWorkflowMigratesWithoutChangingLogic",testSplitStarterWorkflowMigratesWithoutChangingLogic);run("testManualDimensionsAreDurablePresentationOnly",testManualDimensionsAreDurablePresentationOnly);run("testIntegerArithmeticAndConversions",testIntegerArithmeticAndConversions);run("testIntegerTargetPropertiesKeepLegacyConnections",testIntegerTargetPropertiesKeepLegacyConnections);run("testNodeBindings",testNodeBindings);run("testOutputLayers",testOutputLayers);run("testWireBendsArePersistentVisualMetadata",testWireBendsArePersistentVisualMetadata);}
    catch(const std::exception& error){std::cerr<<"Node layout test failed: "<<error.what()<<'\n';return 1;}
    try{run("testGeometryInputsAndLegacyPortMigration",testGeometryInputsAndLegacyPortMigration);run("testWorkflowRegressions",testWorkflowRegressions);run("testDefaultExecution",testDefaultExecution);run("testPayloadAndFilters",testPayloadAndFilters);run("testPerNodeSettingsAndPorts",testPerNodeSettingsAndPorts);run("testBranchActivityAndBypass",testBranchActivityAndBypass);run("testKeyEvents",testKeyEvents);run("testCapturedInputDoesNotCreateEdges",testCapturedInputDoesNotCreateEdges);run("testIncompleteAndValidation",testIncompleteAndValidation);run("testMovementMetadataAndTiming",testMovementMetadataAndTiming);run("testPersistence",testPersistence);run("testWireTiesArePersistentVisualMetadata",testWireTiesArePersistentVisualMetadata);run("testDisableIsDistinctFromBypassAndPersists",testDisableIsDistinctFromBypassAndPersists);run("testRealSignalTelemetryAndBriefEdges",testRealSignalTelemetryAndBriefEdges);run("testFrameSignalsIgnoreRepaintsAndMonitoringCanSleep",testFrameSignalsIgnoreRepaintsAndMonitoringCanSleep);run("testNewInputEventsPropagateThroughUnchangedValues",testNewInputEventsPropagateThroughUnchangedValues);run("testVisionExecutionAndLaziness",testVisionExecutionAndLaziness);run("testVisionDepthOrderingAndOptionalOutputs",testVisionDepthOrderingAndOptionalOutputs);run("testVisionOptionalDepthFailure",testVisionOptionalDepthFailure);run("testMultipleInferenceAndCaptureConstraint",testMultipleInferenceAndCaptureConstraint);run("testVisionProviderFailureIsolation",testVisionProviderFailureIsolation);run("testConcurrentVisionAndControl",testConcurrentVisionAndControl);run("testScreenshotOutput",testScreenshotOutput);std::cout<<"Node graph tests passed\n";return 0;}
    catch(const std::exception& error){std::cerr<<"Node graph test failed: "<<error.what()<<'\n';return 1;}
}
