#pragma once
namespace
{
void testStandardOutputTelemetryIncludesMovementBatches()
{
    Service service;Fixture fixture;auto workspace=defaults();publish(service,workspace);
    const auto actions=service.evaluate(fixture.context);const auto telemetry=service.telemetry();
    require(actions.aim&&!actions.movements.empty(),"Standard Aim fixture produced no movement output");
    require(telemetry.actions.movements.size()==actions.movements.size(),"Telemetry dropped the computed movement batches");
    require(telemetry.actions.movements[0].steps.size()==actions.movements[0].steps.size(),"Telemetry dropped computed movement steps");
}
void testStandardPercentageFovSurvivesMissingInputs()
{
    using K=NodeKind;Service service;VisionFixture fixture;auto workspace=defaults();
    settings(workspace,K::FilterCircle,"enabled",1);settings(workspace,K::FilterCircle,"radius_percent",100);
    for(const auto kind:{K::CaptureSource,K::Inference})
    {
        settings(workspace,kind,"enabled",0);publish(service,workspace);
        require(!service.evaluateVision(fixture.context).visionDetections,"Missing capture/inference reached the standard Vision output");
        settings(workspace,kind,"enabled",1);publish(service,workspace);
        require(static_cast<bool>(service.evaluateVision(fixture.context).visionDetections),"Standard Vision did not recover after its provider was re-enabled");
    }
    auto& graph=workspace.graphs[3];const auto original=graph.links;
    const auto circle=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const Node& n){return n.kind==K::FilterCircle;});
    graph.links.erase(std::remove_if(graph.links.begin(),graph.links.end(),[&](const Link& l){return l.toNode==circle->id&&l.toPort==0;}),graph.links.end());
    publish(service,workspace);require(!service.evaluateVision(fixture.context).visionDetections,"A disconnected percent-FOV input reached Vision output");
    graph.links=original;publish(service,workspace);
    fixture.context.infer=[](int,const FrameStream&,const Node&)->DetectionStream{throw std::runtime_error("Inference fixture failed");};
    require(!service.evaluateVision(fixture.context).visionDetections,"Failed inference reached Vision output");
    const auto output=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const Node& n){return n.kind==K::VisionOutput;});
    require(sampleNode(service.telemetry(),3,output->id).value.error=="Inference fixture failed","Percent FOV hid the original provider error");
}
void testStandardOutputsPreserveEnableErrors()
{
    using K=NodeKind;Service service;Fixture control;VisionFixture vision;auto workspace=defaults();
    const K outputs[]={K::AimOutput,K::AutoShootOutput,K::RecoilOutput,K::VisionOutput};int identifiers[4]{};
    for(int g=0;g<4;++g)
    {
        auto& graph=workspace.graphs[g];for(const auto& node:graph.nodes)if(node.kind==outputs[g])identifiers[g]=node.id;
        require(identifiers[g]!=0,"A standard workflow lost its output");
        auto divide=defaultNode(K::Divide,900);divide.inputValues={{0,1},{1,0}};graph.nodes.push_back(divide);
        graph.nodes.push_back(defaultNode(K::Greater,901));
        graph.links.erase(std::remove_if(graph.links.begin(),graph.links.end(),[&](const Link& l){return l.toNode==identifiers[g]&&l.toPort==0;}),graph.links.end());
        graph.links.push_back({900,0,901,0});graph.links.push_back({901,0,identifiers[g],0});
    }
    publish(service,workspace);require(!service.evaluateVision(vision.context).visionDetections,"An invalid Vision enable still published detections");
    const auto actions=service.evaluate(control.context);
    require(!actions.aim&&!actions.autoShoot&&!actions.recoil&&actions.movements.empty(),"An invalid output enable emitted a control action");
    require(control.predictions==0&&vision.inferences==0,"An invalid output enable evaluated its unused data branch");
    for(int g=0;g<4;++g)
    {
        const auto value=sampleNode(service.telemetry(),g,identifiers[g]).value;
        require(!value.valid&&value.error=="Float division by zero.","A standard output dropped its Enable error");
    }
}
}
