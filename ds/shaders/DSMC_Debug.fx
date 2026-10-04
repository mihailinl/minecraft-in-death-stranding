// DSMC overlay drawing: Minecraft's aim target (a glowing block frame where the next block goes, or around the block
// the crosshair is on) and calibration pins, all projected by the DSMC add-on and depth-tested against DS.
// Pins: draws world-space pins projected by the DSMC add-on.
// Green = projection assuming CameraEntity::FOV is vertical, magenta = assuming it is horizontal.
// A pin that stays glued to the spot where it was dropped while the camera orbits means the pose and the FOV
// convention are right. Behind scene geometry (depth test against DS's reversed-Z buffer) a pin is drawn dashed.
#include "ReShade.fxh"

uniform float2 DsmcPlanes; // near, far from CameraEntity
uniform float4 PinV0; uniform float4 TopV0;
uniform float4 PinV1; uniform float4 TopV1;
uniform float4 PinV2; uniform float4 TopV2;
uniform float4 PinV3; uniform float4 TopV3;
uniform float4 PinH0; uniform float4 TopH0;
uniform float4 PinH1; uniform float4 TopH1;
uniform float4 PinH2; uniform float4 TopH2;
uniform float4 PinH3; uniform float4 TopH3;

// Aim cube corners (uv, view depth, visible), corner i = dx + 2 dy + 4 dz; AimMode 0 none, 1 placement, 2 hit block.
uniform float4 AimC0; uniform float4 AimC1; uniform float4 AimC2; uniform float4 AimC3;
uniform float4 AimC4; uniform float4 AimC5; uniform float4 AimC6; uniform float4 AimC7;
uniform float AimMode;
// Cursor while Minecraft's inventory is open: uv, on
uniform float4 Cursor;
uniform float Timer < source = "timer"; >;

float scene_view_z(float2 uv)
{
	float d = tex2Dlod(ReShade::DepthBuffer, float4(uv, 0, 0)).x; // reversed Z: 1 near, 0 far
	float n = max(DsmcPlanes.x, 1e-3), f = max(DsmcPlanes.y, n + 1.0);
	return n * f / (d * (f - n) + n);
}

// distance in pixels from p to segment a-b
float seg_dist(float2 p, float2 a, float2 b)
{
	float2 ab = b - a, ap = p - a;
	float t = saturate(dot(ap, ab) / max(dot(ab, ab), 1e-6));
	return length(ap - ab * t);
}

void draw_pin(float2 px, float4 base, float4 top, float3 colour, inout float3 c)
{
	if (base.w < 0.5)
		return;
	float2 a = base.xy * BUFFER_SCREEN_SIZE, b = top.xy * BUFFER_SCREEN_SIZE;
	float seg = seg_dist(px, a, b);
	float ring = abs(length(px - a) - 9.0);
	if (seg > 2.5 && ring > 1.6)
		return;
	float2 uv = px / BUFFER_SCREEN_SIZE;
	// pin depth along the post: interpolate between base and top
	float2 ab = b - a;
	float t = saturate(dot(px - a, ab) / max(dot(ab, ab), 1e-6));
	float pin_z = lerp(base.z, top.z, t);
	bool hidden = scene_view_z(uv) < pin_z - 0.25;
	if (hidden && frac((px.x + px.y) / 12.0) < 0.5)
		return;
	c = lerp(c, colour, hidden ? 0.55 : 1.0);
}

// one cube edge: a bright core and a soft halo; behind DS geometry it is dashed and dimmer
void draw_edge(float2 px, float4 a4, float4 b4, float3 colour, inout float3 c)
{
	float2 a = a4.xy * BUFFER_SCREEN_SIZE, b = b4.xy * BUFFER_SCREEN_SIZE;
	float2 ab = b - a;
	float t = saturate(dot(px - a, ab) / max(dot(ab, ab), 1e-6));
	float dist = length(px - a - ab * t);
	if (dist > 6.0)
		return;
	float edge_z = lerp(a4.z, b4.z, t);
	bool hidden = scene_view_z(px / BUFFER_SCREEN_SIZE) < edge_z - 0.08;
	if (hidden && frac((px.x + px.y) / 10.0) < 0.5)
		return;
	float core = 1.0 - smoothstep(1.0, 2.2, dist);
	float halo = (1.0 - smoothstep(2.2, 6.0, dist)) * 0.35;
	c = lerp(c, colour, saturate(core + halo) * (hidden ? 0.5 : 1.0));
}

float3 PS_Pins(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float3 c = tex2D(ReShade::BackBuffer, uv).rgb;
	float2 px = pos.xy;
	if (AimMode > 0.5)
	{
		float pulse = 0.85 + 0.15 * sin(Timer * 0.006);
		float3 col = (AimMode < 1.5 ? float3(1.0, 1.0, 1.0) : float3(1.0, 0.85, 0.3)) * pulse;
		draw_edge(px, AimC0, AimC1, col, c); draw_edge(px, AimC2, AimC3, col, c);
		draw_edge(px, AimC4, AimC5, col, c); draw_edge(px, AimC6, AimC7, col, c);
		draw_edge(px, AimC0, AimC2, col, c); draw_edge(px, AimC1, AimC3, col, c);
		draw_edge(px, AimC4, AimC6, col, c); draw_edge(px, AimC5, AimC7, col, c);
		draw_edge(px, AimC0, AimC4, col, c); draw_edge(px, AimC1, AimC5, col, c);
		draw_edge(px, AimC2, AimC6, col, c); draw_edge(px, AimC3, AimC7, col, c);
	}
	draw_pin(px, PinV0, TopV0, float3(0.1, 1.0, 0.2), c);
	draw_pin(px, PinV1, TopV1, float3(0.1, 1.0, 0.2), c);
	draw_pin(px, PinV2, TopV2, float3(0.1, 1.0, 0.2), c);
	draw_pin(px, PinV3, TopV3, float3(0.1, 1.0, 0.2), c);
	draw_pin(px, PinH0, TopH0, float3(1.0, 0.1, 0.9), c);
	draw_pin(px, PinH1, TopH1, float3(1.0, 0.1, 0.9), c);
	draw_pin(px, PinH2, TopH2, float3(1.0, 0.1, 0.9), c);
	draw_pin(px, PinH3, TopH3, float3(1.0, 0.1, 0.9), c);
	if (Cursor.z > 0.5)
	{
		// an arrow cursor: white with a dark outline, tip at the cursor position
		float2 p = px - Cursor.xy * BUFFER_SCREEN_SIZE;
		bool inner = p.x >= 0.0 && p.y >= 0.0 && p.y <= 20.0 && p.x <= p.y * 0.62 && !(p.y > 15.0 && p.x > (20.0 - p.y) * 1.6);
		bool outer = p.x >= -1.5 && p.y >= -1.5 && p.y <= 22.0 && p.x <= p.y * 0.62 + 2.0 && !(p.y > 16.5 && p.x > (22.0 - p.y) * 1.6 + 1.0);
		if (inner)
			c = float3(1.0, 1.0, 1.0);
		else if (outer)
			c = float3(0.05, 0.05, 0.05);
	}
	return c;
}

technique DSMC_Debug
{
	pass
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Pins;
	}
}
