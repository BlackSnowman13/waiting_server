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
        
    import re
    with open(html_path, "r", encoding="utf-8") as f:
        html = f.read()

    # Minify HTML & CSS: strip comments and collapse whitespace
    html = re.sub(r'/\*.*?\*/', '', html, flags=re.DOTALL)
    html = re.sub(r'>\s+<', '><', html)
    html = re.sub(r'\s+', ' ', html)

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
        "<div id='alert-banner' class='alert-banner'>"
        "<div class='alert-content'>"
        "<div class='alert-title'>Connection Failed</div>"
        "<div class='alert-msg'>%s</div>"
        "</div>"
        "<button type='button' class='alert-close' onclick='dismissAlert()'>&times;</button>"
        "</div>"
    )

    save_response_html = (
        "HTTP/1.1 200 OK\\r\\n"
        "Content-Type: text/html\\r\\n"
        "Connection: close\\r\\n\\r\\n"
        "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{background:#09090b;color:#f4f4f5;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;margin:0;padding:24px 16px;display:flex;justify-content:center;align-items:center;min-height:100vh}*{box-sizing:border-box}.sheet{width:100%;max-width:380px;background:#141416;border:1px solid #222227;border-radius:14px;padding:28px 24px;text-align:center}.icon{display:inline-flex;width:40px;height:40px;border-radius:50%;background:#064e3b;color:#10b981;align-items:center;justify-content:center;font-size:18px;margin-bottom:14px}h2{font-size:17px;font-weight:600;margin:0 0 8px;color:#fafafa}p{font-size:13px;color:#a1a1aa;line-height:1.5;margin:0 0 14px}.note{font-size:11px;color:#71717a}</style></head><body>"
        "<div class='sheet'>"
        "<div class='icon'>&#10003;</div>"
        "<h2>Settings Saved</h2>"
        "<p>WaitingServer is rebooting to connect to your Wi-Fi network.</p>"
        "<div class='note'>The LED will resume its normal pulse once online.</div>"
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
