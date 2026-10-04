package dev.rehan.passthrough.client.mixin;

import net.minecraft.client.MouseHandler;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

/** The host moves Minecraft's cursor (its window never has the focus), so hover, tooltips and the carried item follow. */
@Mixin(MouseHandler.class)
public interface MouseHandlerAccessor {
	@Accessor("xpos")
	void passthrough$setXpos(double x);

	@Accessor("ypos")
	void passthrough$setYpos(double y);
}
