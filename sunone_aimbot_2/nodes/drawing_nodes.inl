// Included by node_graph.cpp inside NodeGraph's implementation namespace.
bool drawingPrimitive(K kind){return kind>=K::DrawPoint&&kind<=K::DrawTextPrimitive;}
bool pointListNode(K kind){return kind>=K::AppendPoint&&kind<=K::PointTrail;}
void addDrawingDefinitions(std::vector<Definition>& defs)
{
    using P=ParameterKind;
    defs.push_back({K::DrawingStyle,"Drawing style","Drawing","Color, width, fill and text size shared by drawing nodes.",{{"Style",T::DrawingStyle}},{{"Style",T::DrawingStyle}},
        {{"draw_r","Red",90,0,255,P::Integer},{"draw_g","Green",220,0,255,P::Integer},{"draw_b","Blue",255,0,255,P::Integer},{"draw_a","Alpha",255,0,255,P::Integer},
        {"draw_width","Line width (px)",1.5,0.1,32},{"draw_fill","Filled",0,0,1,P::Boolean},{"draw_font_size","Text size (px)",16,6,128},{"draw_fade","Trail fade",0,0,1}}});
    const std::vector<ParameterSpec> enabled={{"enabled","Enabled",1,0,1,P::Boolean}};
    for(const auto entry:{std::pair{K::DrawPoint,"Draw point"},std::pair{K::DrawLine,"Draw line"},std::pair{K::DrawBox,"Draw box"},std::pair{K::DrawCircle,"Draw circle"},std::pair{K::DrawPolyline,"Draw polyline"},std::pair{K::DrawTextPrimitive,"Draw text"}})
    {
        Definition def{entry.first,entry.second,"Drawing","Append a primitive to Drawing. Unconnected Drawing starts an empty list; an unconnected Style uses the default drawing style.",{{"Drawing",T::Drawing},{"Position",T::Point}},{{"Drawing",T::Drawing}},enabled};
        if(entry.first==K::DrawLine)def.inputs.push_back({"End",T::Point});
        else if(entry.first==K::DrawBox)def.inputs.push_back({"Size",T::Point});
        else if(entry.first==K::DrawPolyline){def.inputs[1]={"Points",T::Points};def.parameters.push_back({"closed","Closed",0,0,1,P::Boolean});}
        else if(entry.first==K::DrawTextPrimitive){def.inputs.push_back({"Text",T::String,"text"});def.textParameters={{"text","Text","Text",1024}};}
        if(entry.first==K::DrawPoint||entry.first==K::DrawCircle)def.parameters.push_back({"radius","Radius (px)",entry.first==K::DrawPoint?3.0:12.0,0.1,10000});
        def.inputs.push_back({"Style",T::DrawingStyle});defs.push_back(std::move(def));
    }
    defs.push_back({K::MergeDrawings,"Merge drawings","Drawing","Combine two drawing branches, preserving their order.",{{"A",T::Drawing},{"B",T::Drawing}},{{"Drawing",T::Drawing}}});
    defs.push_back({K::RenderDrawing,"Render drawing","Drawing","Draw graph primitives on the game overlay. Capture coordinates use Canvas size or the current capture dimensions. Geometry is evaluated only at the requested output FPS.",
        {{"Enable",T::Boolean,"enabled"},{"Drawing",T::Drawing},{"Canvas size",T::Point}},{{"Rendered",T::Boolean}},
        {{"enabled","Enabled",1,0,1,P::Boolean},{"game_overlay_max_fps","Output FPS",60,1,240,P::Integer},{"coordinate_space","Coordinates",0,0,2,P::Integer,"Capture pixels|Screen pixels|Normalized screen"}}});
    defs.push_back({K::AppendPoint,"Append point","Point lists","Append Point to Points. An unconnected list starts empty. Lists contain at most 256 points.",{{"Points",T::Points},{"Point",T::Point}},{{"Points",T::Points}}});
    defs.push_back({K::MergePoints,"Merge points","Point lists","Join two point lists in order.",{{"A",T::Points},{"B",T::Points}},{{"Points",T::Points}}});
    defs.push_back({K::PointAt,"Point at","Point lists","Read a zero-based point. An out-of-range index stops this branch.",{{"Points",T::Points},{"Index",T::Integer,"index"}},{{"Point",T::Point}},{{"index","Index",0,0,255,P::Integer}}});
    defs.push_back({K::PredictPath,"Predict path","Point lists","Sample Position + projected Velocity over the horizon. Connect Velocity damping and Motion ready to match Project point.",
        {{"Position",T::Point},{"Velocity",T::Point}},{{"Points",T::Points}},{{"seconds","Horizon (sec)",0.25,0,1.5},{"samples","Samples",16,2,256,P::Integer},{"damping","Damping",0,0,3},{"ready","Ready",1,0,1,P::Boolean}}});
    defs.push_back({K::PointTrail,"Point trail","Point lists","Keep a bounded history of Point. Reset or a changed Key clears it. Sampling is independent of the control loop rate.",
        {{"Point",T::Point},{"Reset",T::Boolean,"reset"},{"Key",T::Integer,"key"}},{{"Points",T::Points}},
        {{"reset","Reset",0,0,1,P::Boolean},{"key","Key",0,MinInteger,MaxInteger,P::Integer},{"samples","Maximum points",128,1,256,P::Integer},{"sample_fps","Sample FPS",60,1,240,P::Integer},{"lifetime","Lifetime (sec)",2,0.05,30}}});
    defs.push_back({K::ReroutePoints,"Reroute points","Routing","Route a point list.",{{"Points",T::Points}},{{"Points",T::Points}}});
    defs.push_back({K::RerouteDrawing,"Reroute drawing","Routing","Route graph drawing commands.",{{"Drawing",T::Drawing}},{{"Drawing",T::Drawing}}});
    for(const auto entry:{std::pair{K::GetPoints,T::Points},std::pair{K::GetDrawing,T::Drawing}})
    {
        const bool points=entry.second==T::Points;
        defs.push_back({entry.first,points?"Get points":"Get drawing","Named values","Read a named value from this graph.",{},{{points?"Points":"Drawing",entry.second}},{},{{"name","Name",points?"points":"drawing",128}}});
        defs.push_back({static_cast<K>(static_cast<int>(entry.first)+1),points?"Set points":"Set drawing","Named values","Share this value by name inside the graph.",{{points?"Points":"Drawing",entry.second}},{{points?"Points":"Drawing",entry.second}},{},{{"name","Name",points?"points":"drawing",128}}});
    }
    defs.push_back({K::RerouteDrawingStyle,"Reroute drawing style","Routing","Route a drawing style.",{{"Style",T::DrawingStyle}},{{"Style",T::DrawingStyle}}});
    defs.push_back({K::GetDrawingStyle,"Get drawing style","Named values","Read the named drawing style in this graph.",{},{{"Style",T::DrawingStyle}},{},{{"name","Name","drawing_style",128}}});
    defs.push_back({K::SetDrawingStyle,"Set drawing style","Named values","Share this style by name in the graph.",{{"Style",T::DrawingStyle}},{{"Style",T::DrawingStyle}},{},{{"name","Name","drawing_style",128}}});
}
std::uint64_t drawingStamp(const Node& node,const std::array<Value,MaxDataPins>& inputs,std::size_t count)
{
    std::uint64_t stamp=14695981039346656037ull;
    const auto mix=[&](std::uint64_t value){stamp=(stamp^value)*1099511628211ull;};
    const auto scalar=[&](double value){std::uint64_t bits;std::memcpy(&bits,&value,sizeof(bits));mix(bits);};
    for(std::size_t pin=0;pin<count;++pin)
    {
        const auto& value=inputs[pin];mix(value.valid);scalar(value.point.x);scalar(value.point.y);scalar(value.number);mix(value.integer);mix(value.boolean);
        mix(reinterpret_cast<std::uintptr_t>(value.points.get()));mix(reinterpret_cast<std::uintptr_t>(value.drawing.get()));mix(reinterpret_cast<std::uintptr_t>(value.settings.get()));
        for(unsigned char c:value.text)mix(c);
    }
    for(const auto& entry:node.parameters)scalar(entry.second);
    return stamp;
}
template<class State>
Value evaluateDrawingNode(const Node& node,const std::array<Value,MaxDataPins>& inputs,State& state,double elapsed)
{
    const auto kind=node.kind;Value output;output.type=kind==K::PointAt?T::Point:pointListNode(kind)?T::Points:T::Drawing;
    if(kind==K::PointTrail)
    {
        const int key=inputs[2].integer;bool changed=false;
        if(inputs[1].boolean||!state.hasKey||state.key!=key||elapsed<state.sampledAt)
        {state.trail.clear();state.sampledAt=-1;state.key=key;state.hasKey=true;changed=true;}
        const double oldest=elapsed-parameter(node,"lifetime");const auto expired=std::find_if(state.trail.begin(),state.trail.end(),[&](const auto& entry){return entry.second>=oldest;});
        if(expired!=state.trail.begin()){state.trail.erase(state.trail.begin(),expired);changed=true;}
        if(state.sampledAt<0||elapsed-state.sampledAt>=1.0/parameter(node,"sample_fps"))
        {
            state.sampledAt=elapsed;const auto p=inputs[0].point;
            if(state.trail.empty()||state.trail.back().first.x!=p.x||state.trail.back().first.y!=p.y){state.trail.push_back({p,elapsed});changed=true;}
        }
        const auto limit=static_cast<std::size_t>(parameter(node,"samples"));
        if(state.trail.size()>limit){state.trail.erase(state.trail.begin(),state.trail.end()-limit);changed=true;}
        if(!changed&&state.built)return state.output;
        auto points=std::make_shared<std::vector<Point>>();points->reserve(state.trail.size());for(const auto& entry:state.trail)points->push_back(entry.first);
        output.points=std::move(points);state.output=output;state.built=true;return output;
    }
    const auto stamp=drawingStamp(node,inputs,definition(kind).dataInputs);
    if(state.built&&state.stamp==stamp)return state.output;
    if(kind==K::PointAt)
    {
        const auto index=static_cast<std::size_t>(inputs[1].integer);
        if(!inputs[0].points||index>=inputs[0].points->size())throw std::runtime_error("Point index is outside the list.");
        output.point=(*inputs[0].points)[index];
    }
    else if(pointListNode(kind))
    {
        auto points=std::make_shared<std::vector<Point>>();
        if(kind==K::PredictPath)
        {
            const int count=static_cast<int>(parameter(node,"samples"));points->reserve(count);
            for(int sample=0;sample<count;++sample)points->push_back(projectPoint(inputs[0].point,inputs[1].point,parameter(node,"seconds")*sample/(count-1),parameter(node,"damping"),parameter(node,"ready")!=0));
        }
        else
        {
            if(inputs[0].points)*points=*inputs[0].points;
            if(kind==K::AppendPoint)points->push_back(inputs[1].point);
            else if(inputs[1].points)points->insert(points->end(),inputs[1].points->begin(),inputs[1].points->end());
        }
        if(points->size()>MaxPathPoints)throw std::runtime_error("Point list limit reached (256).");
        output.points=std::move(points);
    }
    else
    {
        if(drawingPrimitive(kind)&&parameter(node,"enabled")==0){output.drawing=inputs[0].drawing;return output;}
        auto drawing=std::make_shared<DrawingData>();if(inputs[0].drawing)*drawing=*inputs[0].drawing;
        if(kind==K::MergeDrawings)
        {
            if(inputs[1].drawing){drawing->commands.insert(drawing->commands.end(),inputs[1].drawing->commands.begin(),inputs[1].drawing->commands.end());drawing->vertices+=inputs[1].drawing->vertices;}
        }
        else
        {
            DrawingCommand command;command.from=inputs[1].point;
            command.style=inputs[definition(kind).dataInputs-1].settings;
            if(kind==K::DrawPoint||kind==K::DrawCircle){command.kind=kind==K::DrawPoint?DrawingCommand::Kind::Point:DrawingCommand::Kind::Circle;command.radius=parameter(node,"radius");}
            else if(kind==K::DrawLine){command.kind=DrawingCommand::Kind::Line;command.to=inputs[2].point;}
            else if(kind==K::DrawBox){command.kind=DrawingCommand::Kind::Box;command.to=inputs[2].point;}
            else if(kind==K::DrawTextPrimitive)
            {
                if(inputs[2].text.size()>1024)throw std::runtime_error("Drawing text limit reached (1024 UTF-8 bytes).");
                command.kind=DrawingCommand::Kind::Text;
                if(state.output.drawing&&!state.output.drawing->commands.empty())
                {const auto& previous=state.output.drawing->commands.back();if(previous.text&&*previous.text==inputs[2].text)command.text=previous.text;}
                if(!command.text)command.text=std::make_shared<const std::string>(inputs[2].text);
            }
            else{command.kind=DrawingCommand::Kind::Polyline;command.points=inputs[1].points;command.closed=parameter(node,"closed")!=0;}
            drawing->vertices+=command.points?command.points->size():command.kind==DrawingCommand::Kind::Line?2:1;
            drawing->commands.push_back(std::move(command));
        }
        if(drawing->commands.size()>MaxDrawingCommands||drawing->vertices>MaxDrawingVertices)throw std::runtime_error("Drawing limit reached (256 commands / 4096 vertices).");
        output.drawing=std::move(drawing);
    }
    for(std::size_t pin=0;pin<definition(kind).dataInputs;++pin)
    {state.pointInputs[pin]=inputs[pin].points;state.drawingInputs[pin]=inputs[pin].drawing;state.styleInputs[pin]=inputs[pin].settings;}
    state.stamp=stamp;state.output=output;state.built=true;return output;
}
