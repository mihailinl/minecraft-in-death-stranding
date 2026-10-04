package dev.rehan.passthrough.client;

import com.google.gson.JsonObject;
import com.mojang.blaze3d.platform.Window;
import dev.rehan.passthrough.Passthrough;
import dev.rehan.passthrough.client.mixin.MouseHandlerAccessor;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.input.MouseButtonEvent;
import com.mojang.blaze3d.platform.InputConstants;
import net.minecraft.client.input.MouseButtonInfo;

/**
 * The host's cursor in Minecraft's screens (inventory): the host draws its own cursor and sends where it is.
 * <ul>
 * <li>{"t":"mouse","x":0..1,"y":0..1}: cursor position as a fraction of the picture</li>
 * <li>{"t":"click","b":0|1|2,"down":bool}</li>
 * <li>{"t":"gscroll","d":+-1}</li>
 * </ul>
 */
final class GuiInput {
	private static int held = -1;
	private static double gx, gy;

	private GuiInput() {
	}

	/** The host's button numbers (0 left, 1 right, 2 middle) -> Minecraft 26.x's, which are SDL3's (left 1, middle 2, right 3). */
	private static int sdlButton(final int host) {
		return switch (host) {
			case 1 -> InputConstants.MOUSE_BUTTON_RIGHT;
			case 2 -> InputConstants.MOUSE_BUTTON_MIDDLE;
			default -> InputConstants.MOUSE_BUTTON_LEFT;
		};
	}

	static void handle(final Minecraft minecraft, final JsonObject m) {
		Window w = minecraft.getWindow();
		Screen screen = minecraft.gui.screen();
		switch (m.get("t").getAsString()) {
			case "mouse" -> {
				double sx = m.get("x").getAsDouble() * w.getScreenWidth(), sy = m.get("y").getAsDouble() * w.getScreenHeight();
				MouseHandlerAccessor mouse = (MouseHandlerAccessor)minecraft.mouseHandler;
				mouse.passthrough$setXpos(sx);
				mouse.passthrough$setYpos(sy);
				double nx = sx * w.getGuiScaledWidth() / w.getScreenWidth(), ny = sy * w.getGuiScaledHeight() / w.getScreenHeight();
				double dx = nx - gx, dy = ny - gy;
				gx = nx;
				gy = ny;
				if (screen != null) {
					screen.mouseMoved(gx, gy);
					if (held >= 0) {
						screen.mouseDragged(new MouseButtonEvent(gx, gy, new MouseButtonInfo(sdlButton(held), 0)), dx, dy);
					}
				}
			}
			case "click" -> {
				int b = m.get("b").getAsInt();
				boolean down = m.get("down").getAsBoolean();
				boolean handled = false;
				if (screen != null) {
					// as MouseHandler.onButton does: the event at the cursor, then afterMouseAction
					MouseButtonEvent event = new MouseButtonEvent(gx, gy, new MouseButtonInfo(sdlButton(b), 0));
					handled = down ? screen.mouseClicked(event, false) : screen.mouseReleased(event);
					screen.afterMouseAction();
				}
				held = down ? b : -1;
				Passthrough.LOG.info("host click b {} {} on {} at {}, {} (handled {})", b, down ? "down" : "up",
					screen == null ? "no screen" : screen.getClass().getSimpleName(), String.format(java.util.Locale.ROOT, "%.1f", gx),
					String.format(java.util.Locale.ROOT, "%.1f", gy), handled);
			}
			case "gscroll" -> {
				if (screen != null) {
					screen.mouseScrolled(gx, gy, 0.0, m.get("d").getAsDouble());
				}
			}
			default -> {
			}
		}
	}
}
