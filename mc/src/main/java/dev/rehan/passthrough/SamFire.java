package dev.rehan.passthrough;

import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.entity.decoration.ArmorStand;

/**
 * The host's player is on fire ({"t":"samfire","on":bool}): an invisible, invulnerable armor stand burns where they
 * stand, so Minecraft draws its flames around them, as on any burning mob. Server thread.
 */
public final class SamFire {
	public static final String TAG = "dsmc_sam_fire";
	private static volatile boolean on;
	private static ArmorStand stand;

	private SamFire() {
	}

	public static void set(final boolean burning) {
		on = burning;
	}

	static void tick(final MinecraftServer server) {
		double[] feet = Passthrough.hostFeet;
		if (!on || !Passthrough.active || feet == null) {
			if (stand != null) {
				stand.discard();
				stand = null;
			}
			return;
		}

		ServerLevel level = server.overworld();
		if (stand == null || stand.isRemoved()) {
			stand = new ArmorStand(level, feet[0], feet[1], feet[2]);
			stand.setInvisible(true);
			stand.setNoGravity(true);
			stand.setSilent(true);
			stand.setNoBasePlate(true);
			stand.setPermanentlyInvulnerable(true);
			stand.addTag(TAG);
			level.addFreshEntity(stand);
		}

		stand.snapTo(feet[0], feet[1], feet[2]);
		stand.setRemainingFireTicks(40);
	}
}
