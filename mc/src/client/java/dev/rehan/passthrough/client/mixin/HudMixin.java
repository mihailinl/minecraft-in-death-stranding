package dev.rehan.passthrough.client.mixin;

import dev.rehan.passthrough.Passthrough;
import dev.rehan.passthrough.client.HostState;
import net.minecraft.client.gui.Hud;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** No crosshair while the host aims from its player's body (third-person building): the host draws the target. */
@Mixin(Hud.class)
abstract class HudMixin {
	@Inject(method = "extractCrosshair", at = @At("HEAD"), cancellable = true)
	private void passthrough$noCrosshair(final CallbackInfo ci) {
		if (Passthrough.active && HostState.crosshairHidden) {
			ci.cancel();
		}
	}
}
