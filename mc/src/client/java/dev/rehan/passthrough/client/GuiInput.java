package dev.rehan.passthrough.client;

import com.google.gson.JsonObject;
import com.mojang.blaze3d.platform.Window;
import dev.rehan.passthrough.client.mixin.MouseHandlerAccessor;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.input.MouseButtonEvent;
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
						screen.mouseDragged(new MouseButtonEvent(gx, gy, new MouseButtonInfo(held, 0)), dx, dy);
					}
				}
			}
			case "click" -> {
				int b = m.get("b").getAsInt();
				boolean down = m.get("down").getAsBoolean();
				if (screen != null) {
					MouseButtonEvent event = new MouseButtonEvent(gx, gy, new MouseButtonInfo(b, 0));
					if (down) {
						screen.mouseClicked(event, false);
					} else {
						screen.mouseReleased(event);
					}
				}
				held = down ? b : -1;
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
