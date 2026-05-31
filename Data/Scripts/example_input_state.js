// Example: read current virtual controller state from the emulated N64 controller ports
// Load via: View -> Scripting Console -> Run Selected

emu.on_frame(() => {
  const states = input.getPortStates();

  states.forEach((state) => {
    if (!state.valid) {
      console.log(`P${state.port}: not ready or no controller response`);
      return;
    }

    console.log(
      `P${state.port}: connected=${state.connected} raw=0x${state.raw.toString(16).padStart(8, "0")} ` +
        `buttons=0x${state.buttons.toString(16).padStart(4, "0")} ` +
        `pressed=[${state.buttonsPressed.join(", ")}] x=${state.x} y=${state.y}`,
    );
  });
});
