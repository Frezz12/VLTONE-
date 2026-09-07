<!-- codebase-memory-mcp:start -->
# Codebase Knowledge Graph (codebase-memory-mcp)

This project uses codebase-memory-mcp to maintain a knowledge graph of the codebase.
ALWAYS prefer MCP graph tools over grep/glob/file-search for code discovery.

## Priority Order
1. `search_graph` — find functions, classes, routes, variables by pattern
2. `trace_path` — trace who calls a function or what it calls
3. `get_code_snippet` — read specific function/class source code
4. `query_graph` — run Cypher queries for complex patterns
5. `get_architecture` — high-level project summary

## When to fall back to grep/glob
- Searching for string literals, error messages, config values
- Searching non-code files (Dockerfiles, shell scripts, configs)
- When MCP tools return insufficient results or are unavailable

## Examples
- Find a handler: `search_graph(name_pattern=".*OrderHandler.*")`
- Who calls it: `trace_path(function_name="OrderHandler", direction="inbound")`
- Read source: `get_code_snippet(qualified_name="pkg/orders.OrderHandler")`
<!-- codebase-memory-mcp:end -->

## Design skills

The user requested these skills for UI work in this project:

- `apple-design-hig` — desktop conventions, gestures, accessibility and visual review; installed from `dickwu/apple-design-skill` with a unique name.
- `apple-design` — direct manipulation, responsive and interruptible motion; from `emilkowalski/skills`.
- `emil-design-eng` — interaction polish and appropriate use of animation; from `emilkowalski/skills`.

Read the relevant installed `SKILL.md` files and their required references before applying them. They live under `$CODEX_HOME/skills` (normally `~/.codex/skills`). Use the other installed skills from `emilkowalski/skills` when the task matches their purpose; do not load the entire collection for every task.

The desktop app uses Qt Widgets and QPainter. Translate web-specific examples into the existing Qt architecture. For timeline navigation and editing, prioritize precise input, stable anchors, immediate direction changes and measured frame delivery. High-frequency editing and keyboard actions should respond directly; decorative motion must not add input latency.
