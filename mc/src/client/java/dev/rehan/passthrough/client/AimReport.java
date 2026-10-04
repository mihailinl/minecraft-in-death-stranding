package dev.rehan.passthrough.client;

import dev.rehan.passthrough.Passthrough;
import java.util.Locale;
import net.minecraft.client.Minecraft;
import net.minecraft.core.BlockPos;
import net.minecraft.world.item.BlockItem;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;

/**
 * Tells the host what the crosshair is on, so it can draw the target in its own picture (Minecraft's thin outline is
 * lost against a busy host scene): {"t":"aim","hit":[x,y,z],"place":[x,y,z],"block":bool}, or {"t":"aim"} for nothing.
 * Sent only when it changes.
 */
public final class AimReport {
	private static String last = "";

	private AimReport() {
	}

	public static void frame() {
		Minecraft minecraft = Minecraft.getInstance();
		if (!Passthrough.active || minecraft.player == null) {
			return;
		}

		String message;
		if (minecraft.hitResult instanceof BlockHitResult hit && hit.getType() == HitResult.Type.BLOCK) {
			BlockPos at = hit.getBlockPos();
			BlockPos place = at.relative(hit.getDirection());
			boolean block = minecraft.player.getMainHandItem().getItem() instanceof BlockItem;
			message = String.format(Locale.ROOT, "{\"t\":\"aim\",\"hit\":[%d,%d,%d],\"place\":[%d,%d,%d],\"block\":%b}",
				at.getX(), at.getY(), at.getZ(), place.getX(), place.getY(), place.getZ(), block);
		} else {
			message = "{\"t\":\"aim\"}";
		}

		if (!message.equals(last)) {
			last = message;
			Passthrough.events.accept(message);
		}
	}
}
