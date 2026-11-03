#pragma once
class IMouseInput;
namespace GraphInput
{
bool resolveKey(int virtualKey,IMouseInput* input,bool systemDown);
bool keyDown(int virtualKey);
}
