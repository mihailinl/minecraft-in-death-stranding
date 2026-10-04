package dev.rehan.passthrough.client.mixin;

import dev.rehan.passthrough.client.HostState;
import net.minecraft.client.Camera;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * In third person the host camera sits behind and beside the player, so aim along the camera's ray (what is under
 * the crosshair) instead of from the player's eyes. The ray starts level with the player and reaches `range` past.
 * With an aim origin from the host (third-person building), the ray starts there instead, along the camera's view:
 * the host's player no longer stands in front of what is aimed at.
 */
@Mixin(Entity.class)
abstract class EntityPickMixin {
	@Inject(method = "pick(DFZ)Lnet/minecraft/world/phys/HitResult;", at = @At("HEAD"), cancellable = true)
	private void passthrough$cameraRay(final double range, final float a, final boolean withLiquids, final CallbackInfoReturnable<HitResult> cir) {
		HostState.Pose p = HostState.frame();
		if (p == null || !((Object) this instanceof LocalPlayer self)) {
			return;
		}

		Camera camera = Minecraft.getInstance().gameRenderer.mainCamera();
		ClipContext.Fluid fluids = withLiquids ? ClipContext.Fluid.ANY : ClipContext.Fluid.NONE;
		if (p.aimFromBody()) {
			Vec3 origin = new Vec3(p.ax(), p.ay(), p.az());
			Vec3 look = new Vec3(camera.forwardVector()).normalize();
			cir.setReturnValue(self.level().clip(new ClipContext(origin, origin.add(look.scale(range)), ClipContext.Block.OUTLINE, fluids, self)));
			return;
		}

		if (p.firstPerson()) {
			return;
		}

		Vec3 from = camera.position();
		Vec3 dir = new Vec3(camera.forwardVector()).normalize();
		double along = Math.max(0.0, self.getEyePosition(a).subtract(from).dot(dir));
		Vec3 start = from.add(dir.scale(Math.max(0.0, along - 0.4)));
		Vec3 end = from.add(dir.scale(along + range));
		cir.setReturnValue(self.level().clip(new ClipContext(start, end, ClipContext.Block.OUTLINE, fluids, self)));
	}
}
