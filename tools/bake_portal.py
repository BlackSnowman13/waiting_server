#!/usr/bin/env python3
"""
tools/bake_portal.py

Compiles pico_w/portal.html ahead-of-time (AOT) into a C++ header
(pico_w/include/portal_html.hpp) containing escaped string literals.
This eliminates runtime HTML file I/O and parsing overhead on the Pico W.
"""

import os
import sys

def main():
    script_dir = os.path.dirname(os.path.realpath(__file__))
    repo_root = os.path.abspath(os.path.join(script_dir, ".."))
    
    html_path = os.path.join(repo_root, "pico_w", "portal.html")
    output_header = os.path.join(repo_root, "pico_w", "include", "portal_html.hpp")
    
    if not os.path.exists(html_path):
        print(f"Error: {html_path} does not exist", file=sys.stderr)
        sys.exit(1)
        
    with open(html_path, "r", encoding="utf-8") as f:
        html = f.read()

    # Escape CSS/HTML percent signs into printf-safe %%
    html_escaped = html.replace("%", "%%")

    # Replace template placeholders with printf format specifiers
    # Order of replacements:
    html_escaped = html_escaped.replace("{{ERROR_MODAL}}", "%s")
    html_escaped = html_escaped.replace("{{SSID}}", "%s")
    html_escaped = html_escaped.replace("{{PASSWORD}}", "%s")
    html_escaped = html_escaped.replace("{{AUTH_OPTIONS}}", "%s")
    html_escaped = html_escaped.replace("{{ATTEMPTS}}", "%u")
    html_escaped = html_escaped.replace("{{HOSTNAME}}", "%s")
    html_escaped = html_escaped.replace("{{MOTD}}", "%s")
    html_escaped = html_escaped.replace("{{TARGET_HOST}}", "%s")
    html_escaped = html_escaped.replace("{{TARGET_PORT}}", "%u")
    html_escaped = html_escaped.replace("{{TARGET_MAC}}", "%s")

    error_modal_snippet = (
        "<div id='error-modal' class='modal-backdrop'>"
        "<div class='modal-box'>"
        "<h3 class='modal-title'>&#9888; Connection Failed</h3>"
        "<div class='modal-msg'>%s</div>"
        "<button class='modal-btn' onclick='dismissError()'>Dismiss</button>"
        "</div>"
        "</div>"
    )

    save_response_html = (
        "HTTP/1.1 200 OK\\r\\n"
        "Content-Type: text/html\\r\\n"
        "Connection: close\\r\\n\\r\\n"
        "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{background:#0f172a;color:#eee;font-family:sans-serif;text-align:center;padding:40px;}"
        ".card{background:#1e293b;border-radius:14px;padding:30px;max-width:400px;margin:auto;box-shadow:0 4px 16px rgba(0,0,0,0.5);border:1px solid #334155;}"
        "h2{color:#38bdf8;}p{color:#94a3b8;line-height:1.5;}</style></head><body>"
        "<div class='card'>"
        "<h2>&#10004; Settings Saved!</h2>"
        "<p>WaitingServer is rebooting to connect to your Wi-Fi network.</p>"
        "<p>The LED will transition to a steady 1 Hz blink once online.</p>"
        "</div></body></html>"
    )

    # Generate C++ Header Content
    # We use C++11 raw string literal: R"raw_portal_html(...)raw_portal_html"
    header_content = f"""#pragma once

// AUTO-GENERATED FILE - DO NOT EDIT MANUALLY.
// Generated from pico_w/portal.html by tools/bake_portal.py.

namespace waiting_server {{

inline constexpr const char PORTAL_HTML_TEMPLATE[] =
R"raw_portal_html({html_escaped})raw_portal_html";

inline constexpr const char PORTAL_ERROR_MODAL_TEMPLATE[] =
"{error_modal_snippet}";

inline constexpr const char PORTAL_SAVE_SUCCESS_RESPONSE[] =
"{save_response_html}";

}} // namespace waiting_server
"""

    os.makedirs(os.path.dirname(output_header), exist_ok=True)
    with open(output_header, "w", encoding="utf-8") as f:
        f.write(header_content)

    print(f"[BAKE] Successfully generated {output_header} from {html_path}")

if __name__ == "__main__":
    main()
