// Example: emu.input() — blocking prompt
// Asks for two integers and prints their sum.
// Run this script, then type each number in the input bar when prompted.

const a = parseInt(emu.input("First number:"),  10);
const b = parseInt(emu.input("Second number:"), 10);
print(a + " + " + b + " = " + (a + b));
