// Added: HD menu pictures (jkGUIHd.h), drawn by std3D_HdMenuDraw (std3D.c) in two passes.
// mode 0, the picture: tex is the HD picture (RGBA). Where the menu's own 640x480 background
//   (orig, its 8-bit pixels) has a hole (holes: index 0 here and all around it, the character
//   screen's 3D view), the picture leaves the 3D view showing.
// mode 1, the menu: tex is the menu buffer (8-bit). A pixel that still has the background's
//   value is left out, so the picture under it shows; the menu's own drawing (text, buttons,
//   frames) is drawn as the plain menu shader draws it (menu_f.glsl). Index 0, which that
//   shader leaves out to show the black under it, is drawn black over the picture (or, with
//   holes, left out: the 3D view).
uniform sampler2D tex;
uniform sampler2D orig;
uniform sampler2D displayPalette;
uniform int mode;
uniform vec4 origMap; // mode 0: orig's uv = f_uv * origMap.xy + origMap.zw
uniform int holes;
in vec4 f_color;
in vec2 f_uv;
out vec4 fragColor;

float origAt(ivec2 p)
{
    return texelFetch(orig, clamp(p, ivec2(0, 0), ivec2(639, 479)), 0).r;
}

void main(void)
{
    if (mode == 0)
    {
        if (holes != 0)
        {
            vec2 ouv = f_uv * origMap.xy + origMap.zw;
            if (ouv.x >= 0.0 && ouv.x < 1.0 && ouv.y >= 0.0 && ouv.y < 1.0)
            {
                ivec2 p = ivec2(ouv * vec2(640.0, 480.0));
                float m = 0.0;
                for (int dy = -2; dy <= 2; dy += 2)
                    for (int dx = -2; dx <= 2; dx += 2)
                        m = max(m, origAt(p + ivec2(dx, dy)));
                if (m == 0.0)
                    discard;
            }
        }
        fragColor = vec4(texture(tex, f_uv).rgb, 1.0);
        return;
    }

    ivec2 p = ivec2(f_uv * vec2(textureSize(tex, 0)));
    float index = texelFetch(tex, p, 0).r;
    if (p.x < 640 && p.y < 480 && texelFetch(orig, p, 0).r == index)
        discard;
    if (index == 0.0)
    {
        if (holes != 0)
            discard;
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec4 palvald = texture(displayPalette, vec2(index, 0.5));
    fragColor = vec4(palvald.r, palvald.g, palvald.b, 1.0) * f_color;
}
