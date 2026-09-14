// The plugin owns slots, claim precedence and local-selection suppression.
// Only 1..8 and 129..136 belong to this effect; other stencil users are ignored.
// Alpha is occupancy, not opacity. Count cannot index the palette: slots may have holes.
if (ClientCount < 0.5)
    return SceneColor.rgb;

float4 Palette[8] = {Client1, Client2, Client3, Client4, Client5, Client6, Client7, Client8};
int Center = (int)round(Stencil.r);
float2 UV = GetViewportUV(Parameters);
float2 Pixel = View.ViewSizeAndInvSize.zw;
float BestScore = -1.0;
float3 EdgeColor = SceneColor.rgb;
float EdgeOpacity = 0.0;

// Width is a separate visual choice from the web's strength 5/2: two render
// pixels for direct selections, one for descendants, with relative strength 1/0.4.
// A circular 2px kernel avoids the square corners of a box dilation.
[unroll]
for (int Y = -2; Y <= 2; ++Y)
{
    [unroll]
    for (int X = -2; X <= 2; ++X)
    {
        int DistanceSquared = X * X + Y * Y;
        if (DistanceSquared == 0 || DistanceSquared > 4)
            continue;
        float2 SampleUV = UV + float2(X, Y) * Pixel;
        // Do not wrap/clamp an offscreen sample into a false border at the view edge.
        if (any(SampleUV < 0.0) || any(SampleUV >= 1.0))
            continue;
        int Value = (int)round(SceneTextureLookup(ViewportUVToSceneTextureUV(SampleUV, PPI_CustomStencil), PPI_CustomStencil, false).r);
        bool Child = Value >= 129 && Value <= 136;
        int Slot = Child ? Value - 128 : Value;
        if (Slot < 1 || Slot > 8 || Value == Center)
            continue;
        float4 Color = Palette[Slot - 1];
        if (Color.a < 0.5 || (Child && DistanceSquared > 1))
            continue;

        // Nearest boundary wins. At equal distance direct selections outrank
        // descendants; stable iteration resolves the remaining subpixel tie.
        float Score = 10.0 - DistanceSquared + (Child ? 0.0 : 0.25);
        if (Score > BestScore)
        {
            float MarkedDepth = SceneTextureLookup(ViewportUVToSceneTextureUV(SampleUV, PPI_CustomDepth), PPI_CustomDepth, false).r;
            float VisibleDepth = SceneTextureLookup(ViewportUVToSceneTextureUV(SampleUV, PPI_SceneDepth), PPI_SceneDepth, false).r;
            // Both buffers supply linear centimetres. The 1cm bias avoids self-
            // occlusion from depth precision; hidden edges keep the owner's hue.
            bool Occluded = MarkedDepth > VisibleDepth + 1.0;
            BestScore = Score;
            EdgeColor = Color.rgb * View.PreExposure;
            EdgeOpacity = (Child ? 0.4 : 1.0) * (Occluded ? 0.25 : 1.0);
        }
    }
}
return lerp(SceneColor.rgb, EdgeColor, EdgeOpacity);
