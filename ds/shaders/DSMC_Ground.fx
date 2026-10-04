// DSMC ground scan: DS's depth buffer at 256x144, raw (reversed Z, 1 = near, 0 = far / sky).
// The DSMC add-on copies DsmcGroundTex to the CPU every few frames, turns each texel into a point of DS's world
// with the camera of that frame, and sends the points to Minecraft as invisible barrier blocks: the real ground,
// walls and rocks to build on. Draws nothing on screen.
#include "ReShade.fxh"

texture DsmcGroundTex { Width = 256; Height = 144; Format = R32F; };

float PS_GroundScan(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return tex2Dlod(ReShade::DepthBuffer, float4(uv, 0, 0)).x;
}

technique DSMC_Ground < ui_tooltip = "Scans DS's depth for Minecraft's collision (no visible output)."; >
{
	pass
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_GroundScan;
		RenderTarget = DsmcGroundTex;
	}
}
