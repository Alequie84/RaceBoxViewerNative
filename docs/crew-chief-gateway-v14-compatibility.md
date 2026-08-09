# Crew Chief Gateway v14 compatibility

The registered RaceBox Crew Chief service keeps the same Tailscale host, port,
authentication, and request routes. Gateway v14 changes only the server-side
model orchestration: normal Viewer chat, Race Day comparison, companion text,
setup-sheet vision, and official-results vision all call one GPT-5.6 Sol xhigh
agent directly through `racebox-single-agent-v1`.

No Viewer request or persistence contract changes are required. The Viewer
continues to use `/v1/crew-chief/chat`, `racebox-race-day-request-v4`,
`racebox-session-chat-request-v1`, and `racebox-companion-session-v1`. The old
adaptive brain and worker lanes remain server-side only as inactive rollback
material.
