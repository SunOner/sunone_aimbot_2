#pragma once

namespace
{
void numericWheelTick(EditorHarness& h,float delta)
{
    ImGui::GetIO().AddMouseWheelEvent(0,delta);h.frame();requireNoFieldGesture();
}
void numericWheelFixture(EditorHarness& h,NodeKind kind,float zoom=1)
{
    h.graph().nodes={defaultNode(kind,1,32,32)};h.graph().links.clear();h.graph().groups.clear();h.graph().zoom=zoom;
    editor.viewMode=1;edited();editor.undo.clear();editor.redo.clear();h.frame();
}
void testNumericWheelRequiresClickAndCloseZoom()
{
    for(bool local:{false,true})for(float zoom:{0.65f,0.9f,1.0f,1.75f})
    {
        gestureScenario="Wheel activation, local="+std::to_string(local)+", zoom="+std::to_string(zoom);
        EditorHarness h;numericWheelFixture(h,local?NodeKind::Integer:NodeKind::IntegerAdd,zoom);
        const auto field=[&](){return local?inlineField(h,1):defaultField(h,1,0);};
        const auto value=[&](){const auto& n=*findNode(h.graph(),1);const auto found=n.inputValues.find(0);return local?n.value:found==n.inputValues.end()?0:found->second;};
        const double original=value();h.move(field());numericWheelTick(h,-1);
        require(value()==original&&h.graph().zoom<zoom&&!editor.numericWheel.node,"Hover alone edited a number instead of zooming");
        h.graph().zoom=zoom;h.graph().panX=h.graph().panY=0;h.frame();h.click(field());
        numericWheelTick(h,1);
        if(zoom<NumericWheelMinZoom)
        {
            require(value()==original&&h.graph().zoom>zoom&&!editor.numericWheel.node,"A distant value accepted wheel editing");
            const auto after=h.graph().zoom;h.move(field());numericWheelTick(h,1);
            require(value()==original&&h.graph().zoom>=after&&!editor.numericWheel.node,"Zooming past the threshold armed a field without another click");
        }
        else
            require(value()==original+1&&h.graph().zoom==zoom&&editor.numericWheel.node==1,"Clicking a nearby number did not reserve the wheel for that field");
    }
}
void testNumericWheelStepsPrecisionAndHistory()
{
    gestureScenario="Numeric wheel steps, trackpad fractions and grouped Undo";
    EditorHarness h;numericWheelFixture(h,NodeKind::Integer);findNode(h.graph(),1)->value=41;edited();h.frame();
    h.click(inlineField(h,1));const auto before=editor.undo.size();
    for(int i=0;i<3;++i)numericWheelTick(h,0.25f);
    require(findNode(h.graph(),1)->value==41&&editor.undo.size()==before,"Fractional wheel ticks lost integer precision or created empty history");
    numericWheelTick(h,0.25f);numericWheelTick(h,-1);numericWheelTick(h,1);
    h.key(ImGuiMod_Ctrl,true);numericWheelTick(h,1);h.key(ImGuiMod_Ctrl,false);
    require(findNode(h.graph(),1)->value==52&&findNode(service().workspace().graphs[0],1)->value==52&&editor.undo.size()==before+1,
        "Int wheel changes did not publish immediately or formed multiple Undo entries in one burst");
    history(false);h.frame();require(findNode(h.graph(),1)->value==41&&!editor.numericWheel.node,"Undo did not restore the complete wheel burst and disarm it");
    history(true);h.frame();require(findNode(h.graph(),1)->value==52,"Redo lost the complete wheel burst");
    h.click(inlineField(h,1));for(int i=0;i<30;++i)h.frame();numericWheelTick(h,1);
    for(int i=0;i<30;++i)h.frame();numericWheelTick(h,1);
    history(false);h.frame();require(findNode(h.graph(),1)->value==53,"Separate wheel bursts were merged in Undo");

    numericWheelFixture(h,NodeKind::Float);findNode(h.graph(),1)->value=0.005555;edited();h.frame();h.click(inlineField(h,1));
    numericWheelTick(h,1);h.key(ImGuiMod_Shift,true);numericWheelTick(h,1);h.key(ImGuiMod_Shift,false);
    h.key(ImGuiMod_Ctrl,true);numericWheelTick(h,1);h.key(ImGuiMod_Ctrl,false);
    require(std::abs(findNode(h.graph(),1)->value-0.006665)<1e-12,"Float scrolling rounded away existing precision or ignored fine/fast modifiers");
    h.frame();require(std::abs(findNode(h.graph(),1)->value-0.006665)<1e-12,"An old InputDouble text buffer overwrote the wheel value");
    history(false);h.frame();require(std::abs(findNode(h.graph(),1)->value-0.005555)<1e-12,"Float Undo lost the original precision");
    findNode(h.graph(),1)->value=0.99;edited();h.frame();h.click(inlineField(h,1));numericWheelTick(h,1);numericWheelTick(h,1);
    require(std::abs(findNode(h.graph(),1)->value-1.01)<1e-12,"Float step changed abruptly at a decimal boundary within one burst");
}
void testNumericWheelDefaultsBoundsAndConnections()
{
    for(float zoom:{1.0f,1.75f})
    {
        gestureScenario="Wheel defaults and bounds at zoom "+std::to_string(zoom);EditorHarness h;
        numericWheelFixture(h,NodeKind::MakePoint,zoom);h.click(defaultField(h,1,0));const auto before=editor.undo.size();numericWheelTick(h,1);
        require(std::abs(findNode(h.graph(),1)->inputValues.at(0)-0.01)<1e-12&&editor.undo.size()==before+1,"An unwired Float input did not accept a wheel literal");
        history(false);h.frame();require(findNode(h.graph(),1)->inputValues.empty(),"Undo did not remove an implicit Float default override");
        numericWheelFixture(h,NodeKind::FilterConfidence,zoom);findNode(h.graph(),1)->parameters["threshold"]=0.995;edited();h.frame();
        h.click(optionField(h,1,"threshold"));numericWheelTick(h,1);
        const auto count=editor.undo.size();const auto revision=service().revision();numericWheelTick(h,1);
        require(parameter(*findNode(h.graph(),1),"threshold")==1&&editor.undo.size()==count&&service().revision()==revision,"A ranged Float exceeded its limit or kept publishing no-op edits");
        numericWheelTick(h,-1);require(parameter(*findNode(h.graph(),1),"threshold")<1,"A clamped field could not scroll back from its limit");
        numericWheelFixture(h,NodeKind::Integer,zoom);findNode(h.graph(),1)->value=MaxInteger;edited();h.frame();h.click(inlineField(h,1));
        const auto historyCount=editor.undo.size();numericWheelTick(h,10);
        require(findNode(h.graph(),1)->value==MaxInteger&&editor.undo.size()==historyCount,"Int wheel overflowed or added history at its maximum");
        numericWheelTick(h,-1);require(findNode(h.graph(),1)->value==MaxInteger-1,"Int could not scroll down from its maximum");
        numericWheelFixture(h,NodeKind::IntegerAdd,zoom);h.graph().nodes.push_back(defaultNode(NodeKind::Integer,2,448,32));h.graph().links={{2,0,1,0}};
        findNode(h.graph(),1)->inputValues[0]=19;edited();h.frame();h.click(defaultField(h,1,0));numericWheelTick(h,1);
        require(findNode(h.graph(),1)->inputValues.at(0)==19&&!editor.numericWheel.node,"A connected input allowed its stored value to be edited by wheel");
    }
}
void testNumericWheelDisarmsAndRespectsOtherControls()
{
    gestureScenario="Wheel arming ends on leave, Escape, zoom and popup";EditorHarness h;
    numericWheelFixture(h,NodeKind::Integer);h.click(inlineField(h,1));numericWheelTick(h,1);
    h.move(localFieldRow(h,1).label.GetCenter());require(!editor.numericWheel.node,"Leaving the value did not disarm it");
    h.move(inlineField(h,1));numericWheelTick(h,1);require(findNode(h.graph(),1)->value==1&&h.graph().zoom>1,"Returning without a click rearmed the value");
    h.click(inlineField(h,1));h.key(ImGuiKey_Escape,true);h.key(ImGuiKey_Escape,false);
    numericWheelTick(h,1);require(findNode(h.graph(),1)->value==1&&!editor.numericWheel.node,"Escape did not disarm wheel editing");
    h.click(inlineField(h,1));h.graph().zoom=0.9f;h.frame();h.move(inlineField(h,1));numericWheelTick(h,1);
    require(findNode(h.graph(),1)->value==1&&!editor.numericWheel.node,"A previously armed field stayed armed after zooming out");
    numericWheelFixture(h,NodeKind::ToInteger);h.click(optionField(h,1,"rounding"));h.frame();numericWheelTick(h,1);
    require(parameter(*findNode(h.graph(),1),"rounding")==0&&!editor.numericWheel.node,"The numeric wheel changed a choice or leaked through its popup");
    h.key(ImGuiKey_Escape,true);h.key(ImGuiKey_Escape,false);
    numericWheelFixture(h,NodeKind::Integer);h.click(inlineField(h,1));findNode(h.graph(),1)->collapsed=true;h.frame();
    require(!editor.numericWheel.node,"Collapsing a card retained numeric wheel ownership");
}
}
