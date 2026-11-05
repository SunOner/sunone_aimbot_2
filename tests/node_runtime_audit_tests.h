#pragma once
namespace
{
void testEventStateSurvivesUnrelatedLogicEdits()
{
    using K=NodeKind;Fixture fixture;Service service;auto workspace=emptyWorkspace();
    for(std::size_t i=0;i<workspace.graphs.size();++i)workspace.graphs[i].workflow=static_cast<int>(i)+1;
    auto& graph=workspace.graphs[2];auto key=defaultNode(K::KeyHeld,1);key.parameters["virtual_key"]=65;
    graph.nodes={key,defaultNode(K::Toggle,2),defaultNode(K::RecoilOutput,4)};constant(graph,3,1);
    graph.links={{1,0,2,0},{2,0,4,0},{3,0,4,1}};publish(service,workspace);
    require(!service.evaluate(fixture.context).recoil,"Toggle did not start off");
    fixture.keys[65]=true;require(service.evaluate(fixture.context).recoil,"Toggle did not latch");
    node(graph,3).value=7;publish(service,workspace);const auto edited=service.evaluate(fixture.context);
    require(edited.recoil&&edited.recoilStrength==7,"Editing downstream strength reset the Toggle latch");
    fixture.keys[65]=false;service.evaluate(fixture.context);fixture.keys[65]=true;
    require(!service.evaluate(fixture.context).recoil,"Second edge did not unlatch");
    constant(workspace.graphs[0],99,5);publish(service,workspace);
    require(!service.evaluate(fixture.context).recoil,"Editing another workflow synthesized a Toggle edge");
    node(graph,1).parameters["virtual_key"]=66;fixture.keys[66]=true;publish(service,workspace);
    require(!service.evaluate(fixture.context).recoil,"Rebinding a held trigger synthesized a Toggle edge");
    fixture.keys[66]=false;service.evaluate(fixture.context);fixture.keys[66]=true;
    require(service.evaluate(fixture.context).recoil,"Rebound trigger did not rearm");
    fixture.keys[66]=false;service.evaluate(fixture.context);fixture.keys[66]=true;
    require(!service.evaluate(fixture.context).recoil,"Rebound Toggle did not unlatch");
    workspace.graphs.erase(workspace.graphs.begin());publish(service,workspace);
    require(!service.evaluate(fixture.context).recoil,"Removing a preceding workflow moved event state to the wrong slot");
    fixture.keys[66]=false;service.evaluate(fixture.context);fixture.keys[66]=true;
    require(service.evaluate(fixture.context).recoil,"Remapped event state did not rearm");
    // A new workflow identity must not inherit the old workflow's live latch.
    workspace.graphs[1].workflow=900;const auto revision=service.revision();publish(service,workspace);
    require(service.revision()>revision,"Changing workflow identity was treated as a presentation edit");
}
void testUnavailableTriggersDoNotCreateRecoveryEdges()
{
    using K=NodeKind;
    for(const auto kind:{K::KeyPressed,K::KeyReleased})
    {
        Fixture fixture;Service service;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[2];
        auto key=defaultNode(kind,1);key.parameters["virtual_key"]=65;
        graph.nodes={key,defaultNode(K::RecoilOutput,3)};constant(graph,2,1);graph.links={{1,0,3,0},{2,0,3,1}};publish(service,workspace);
        fixture.keys[65]=kind==K::KeyReleased;service.evaluate(fixture.context);
        const auto callback=fixture.context.keyDown;fixture.context.keyDown={};
        require(!service.evaluate(fixture.context).recoil,"Missing key provider did not disable its branch");
        fixture.keys[65]=kind==K::KeyPressed;fixture.context.keyDown=callback;
        require(!service.evaluate(fixture.context).recoil,"Restoring a key provider synthesized a press/release edge");
        fixture.keys[65]=kind!=K::KeyPressed;service.evaluate(fixture.context);fixture.keys[65]=kind==K::KeyPressed;
        require(service.evaluate(fixture.context).recoil,"Real key edges did not rearm after recovery");
    }
    Fixture fixture;Service service;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[2];
    auto key=defaultNode(K::KeyHeld,1);key.parameters["virtual_key"]=65;
    graph.nodes={key,defaultNode(K::Toggle,2),defaultNode(K::RecoilOutput,4)};constant(graph,3,1);graph.links={{1,0,2,0},{2,0,4,0},{3,0,4,1}};publish(service,workspace);
    service.evaluate(fixture.context);fixture.keys[65]=true;require(service.evaluate(fixture.context).recoil,"Toggle did not latch before provider loss");
    const auto callback=fixture.context.keyDown;fixture.context.keyDown={};require(!service.evaluate(fixture.context).recoil,"Unavailable trigger did not stop its output");
    fixture.context.keyDown=callback;require(service.evaluate(fixture.context).recoil,"Trigger recovery flipped the stored Toggle latch");
    fixture.context.keyDown=[](int)->bool{throw std::runtime_error("Key provider lost");};require(!service.evaluate(fixture.context).recoil,"A throwing key provider did not stop its branch");
    fixture.context.keyDown=callback;require(service.evaluate(fixture.context).recoil,"Recovery after a provider exception flipped the latch");
}
void testErrorCausesPropagateAndRecover()
{
    using K=NodeKind;Fixture fixture;Service service;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[2];
    constant(graph,1,8);graph.nodes.push_back(defaultNode(K::CaptureFps,2));graph.nodes.push_back(defaultNode(K::Divide,3));
    graph.nodes.push_back(defaultNode(K::Add,4));graph.nodes.push_back(defaultNode(K::Greater,5));graph.nodes.push_back(defaultNode(K::RecoilOutput,6));
    node(graph,4).inputValues[1]=1;node(graph,5).inputValues[1]=0;
    graph.links={{1,0,3,0},{2,0,3,1},{3,0,4,0},{4,0,5,0},{5,0,6,0},{1,0,6,1}};publish(service,workspace);
    fixture.context.captureFps=0;require(!service.evaluate(fixture.context).recoil,"A division error reached an output");
    const auto failed=service.telemetry();
    for(const int id:{3,4,5,6})require(sampleNode(failed,2,id).value.error=="Float division by zero.","A transform lost the original upstream error cause");
    fixture.context.captureFps=1;std::this_thread::sleep_for(std::chrono::milliseconds(35));require(service.evaluate(fixture.context).recoil,"A corrected input did not restore the branch");
    const auto recovered=service.telemetry();for(const int id:{3,4,5,6})require(sampleNode(recovered,2,id).value.valid&&sampleNode(recovered,2,id).value.error.empty(),"Recovered nodes retained an old error");
    fixture.context.captureFps=0;service.evaluate(fixture.context);node(graph,3).disabled=true;publish(service,workspace);service.evaluate(fixture.context);
    require(sampleNode(service.telemetry(),2,3).value.error.empty(),"A disabled node retained a stale error from its old result");
    node(graph,3).disabled=false;node(graph,2).disabled=true;publish(service,workspace);service.evaluate(fixture.context);
    const auto inactive=service.telemetry();for(const int id:{3,4,5,6})require(!sampleNode(inactive,2,id).value.valid&&sampleNode(inactive,2,id).value.error.empty(),"An unavailable input was misreported as a new division error");
}
void testDormantBranchesDoNotReplayKeyEdges()
{
    using K=NodeKind;
    for(const auto kind:{K::KeyPressed,K::KeyReleased,K::Toggle})
    {
        Fixture fixture;Service service;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[2];
        auto event=defaultNode(kind,2);if(kind!=K::Toggle)event.parameters["virtual_key"]=65;
        graph.nodes={defaultNode(K::AimHeld,1),event,defaultNode(K::SelectBoolean,4),defaultNode(K::RecoilOutput,6)};
        constant(graph,3,0,true);constant(graph,5,1);graph.links={{1,0,4,0},{2,0,4,1},{3,0,4,2},{4,0,6,0},{5,0,6,1}};
        if(kind==K::Toggle){auto key=defaultNode(K::KeyHeld,7);key.parameters["virtual_key"]=65;graph.nodes.push_back(key);graph.links.push_back({7,0,2,0});}
        publish(service,workspace);fixture.keys[65]=kind==K::KeyReleased;require(!service.evaluate(fixture.context).recoil,"Event branch did not start inactive");
        fixture.context.aimHeld=false;fixture.keys[65]=kind!=K::KeyReleased;
        require(!service.evaluate(fixture.context).recoil,"The unchosen event branch executed");
        fixture.context.aimHeld=true;
        require(!service.evaluate(fixture.context).recoil,"Resuming a lazy branch replayed an unobserved key edge");
        fixture.keys[65]=kind==K::KeyReleased;service.evaluate(fixture.context);fixture.keys[65]=kind!=K::KeyReleased;
        require(service.evaluate(fixture.context).recoil,"A lazy event branch did not rearm after resuming");
    }
}
}
