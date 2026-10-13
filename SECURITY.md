# Security and deployment

This is developer tooling, not a hardened public remote-control service.

- The control server intentionally has no app-layer pairing password. Anyone
  with network access to the app-facing routes can request sessions or status,
  and anyone with loopback/tunnel access to the controller can request
  screenshots or input. Run it locally, on Tailscale, or behind an equivalent
  private authenticated transport.
- Local development uses unencrypted ws/HTTP on a trusted network. A VPS needs
  TLS, protected controller access, firewall configuration, and careful proxy
  routing. Do not expose all routes or forward raw development ports publicly.
- A reverse proxy connects from loopback and can defeat loopback-only route
  checks if it forwards administrative paths. Allow only the app paths documented
  in host/control_server/README.md and reject browser origins.
- The board's HTTP input tool requires an input-device secret for mutating
  routes. The control server stores the same secret and sends it to the iPad app
  only after accepting a session. The protocol is plaintext HTTP on your Wi-Fi
  network, with the secret stored in the board's flash (NVS) and sent by the app
  in every mutating request. Anyone on that network who learns the secret can
  inject keyboard and mouse input into the paired device: use a trusted network,
  and reprovision (`host/tools/provision.sh`) if the secret leaks. The Bluetooth LE
  pairing is "Just Works" (no code), like most Bluetooth mice.
- The phone call service (`host/tools/phone_service.py`) listens on loopback only
  and stores call sessions and transcripts under `.state/phoneuse/`.
- Session authorization is in server memory and expires on restart. Pending
  permits expire after two minutes; active sessions end on disconnect. No
  disconnected or uncertain command is automatically retried.
- End Session revokes new commands before waiting for HID cleanup. Network loss
  or a held input can delay confirmation. Unplug the tool when an input stop is
  unconfirmed. Live Activity dismissal alone does not stop input.
- Screenshots can contain sensitive data. The CLI writes them to the private
  state directory until you remove them. Server logs, measurement traces, and
  calibration profiles are also local runtime data, not release fixtures.
- The MCP adapter is no-auth by design for private tunnel prototyping and binds
  to `127.0.0.1` by default. If a Tailscale-native agent needs direct access,
  bind it to the Tailscale IP with `MCP_ALLOW_TAILNET=1` and rely on Tailnet
  ACLs as the access boundary. Do not expose it through Funnel or the public
  internet. Use a private tunnel or add OAuth before remote/public deployment.
- No multi-user isolation, role-based access, per-agent permissions, or
  production rate limiting is implemented yet.

Do not publish `.state/`, `build/`, `.signing.env`, signing certificates,
provisioning profiles, screenshots, or call recordings (`*.wav`); `.gitignore`
excludes them.
