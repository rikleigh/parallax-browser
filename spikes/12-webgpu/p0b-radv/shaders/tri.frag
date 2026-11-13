#version 450
layout(push_constant) uniform PC { vec4 colour; } pc;
layout(location = 0) out vec4 o;
void main() { o = pc.colour; }
