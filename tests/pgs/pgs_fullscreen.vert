#version 450
// One triangle over the whole target, for pgs_scanout_exec.c: the
// renderer draws its circuit the same way (cmd.draw(3)).
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
