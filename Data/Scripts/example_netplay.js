// Example: netplay session overlay
// Prints a session summary on load, then logs a stats line every second.
// Load via: View -> Scripting Console -> Run Selected

// ── On load: one-time session summary ─────────────────────────────────────
if (netplay.isActive()) {
  const mode  = netplay.getMode();
  const total = netplay.getNumPlayers();
  const slot  = netplay.getPlayerNumber();

  const lines = ["=== Netplay session ==="];
  lines.push(`Mode       : ${mode}`);

  for (let i = 1; i <= total; i++) {
    const tag = i === slot ? " (you)" : "";
    lines.push(`  P${i}: ${netplay.getPlayerName(i)}${tag}`);
  }

  if (mode === "rollback") {
    lines.push(`Delay      : ${netplay.getFrameDelay()} frames`);
    lines.push(`Prediction : ${netplay.getPredictionWindow()} frames`);
  } else if (mode === "kaillera") {
    lines.push(`Delay      : ${netplay.getFrameDelay()} frames`);
  }

  lines.forEach(l => console.log(l));
} else {
  console.log("No netplay session active.");
}

// ── Per-frame stats overlay (logs every 60 frames ≈ 1 second) ─────────────
let frame = 0;

emu.on_frame(() => {
  if (!netplay.isActive()) return;
  if (++frame % 60 !== 0) return;

  const mode = netplay.getMode();
  const slot = netplay.getPlayerNumber();

  // Build player list: "Alice (you) vs Bob"
  const total = netplay.getNumPlayers();
  const players = [];
  for (let i = 1; i <= total; i++) {
    players.push(netplay.getPlayerName(i) + (i === slot ? " (you)" : ""));
  }
  const vs = players.join(" vs ");

  if (mode === "rollback") {
    const ping       = netplay.getPing();                  // integer ms
    const avgPing    = netplay.getAvgPing().toFixed(0);
    const jitter     = netplay.getJitter().toFixed(1);
    const delay      = netplay.getFrameDelay();
    const prediction = netplay.getPredictionWindow();
    const rolling    = netplay.isRollingBack() ? " [ROLLBACK]" : "";

    console.log(
      `[rollback] ${vs} | ping ${ping}ms avg ${avgPing}ms jitter ${jitter}ms | delay ${delay}f predict ${prediction}f${rolling}`
    );
  } else if (mode === "kaillera") {
    const delay = netplay.getFrameDelay();

    console.log(
      `[kaillera] ${vs} | delay ${delay}f`
    );
  }
});
