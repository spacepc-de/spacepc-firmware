// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>

namespace PortalTheme {

const char head[] PROGMEM = R"HTML(
<style>
:root{color-scheme:dark;font-family:system-ui,sans-serif;background:#0b1020;color:#f3f6fa}
*{box-sizing:border-box}
body{margin:0;padding:0 24px 32px;background:#0b1020;color:#f3f6fa;text-align:left}
.wrap{display:block;max-width:820px;margin:auto;text-align:left}
.spc-brandbar{height:64px;display:flex;align-items:center;border-bottom:1px solid #263247;margin-bottom:32px}
.spc-brand{color:#f3f6fa;text-decoration:none;font-weight:800;letter-spacing:-.02em;font-size:1.15rem}
.spc-brand span{color:#168bff}
.spc-intro{margin:0 0 24px;color:#9aa7b8}
h1{font-size:2rem;line-height:1.15;margin:0 0 8px;color:#f3f6fa}
h2{font-size:1.1rem;margin:26px 0 4px;padding:0 0 12px;border-bottom:1px solid #263247;color:#f3f6fa}
h3{color:#9aa7b8;font-weight:400;margin:0 0 24px}
label{display:block;margin:12px 0 5px;font-weight:600;color:#f3f6fa}
input,select{width:100%;padding:11px;margin:0;border:1px solid #475569;border-radius:6px;background:#0b1020;color:#f3f6fa}
input:focus,select:focus{outline:2px solid #168bff;outline-offset:2px;border-color:#168bff}
input[type=checkbox]{width:auto;margin-right:8px}
button,input[type=button],input[type=submit]{width:100%;border:0;border-radius:6px;background:#168bff;color:#fff;font-weight:700;padding:11px 18px;line-height:1.5;cursor:pointer}
button:hover,input[type=submit]:hover{background:#0878e5}
form{border:1px solid #334155;background:#121a2b;padding:22px;margin:0 0 16px;border-radius:8px}
form form{border:0;padding:0;margin:0;background:transparent}
hr{border:0;border-top:1px solid #263247;margin:20px 0}
a{color:#55c7ff}.msg{border-left:3px solid #168bff;background:#10192a;padding:12px 14px;border-radius:0}
.D{background:#44202a}.D:hover{background:#642c39}
.spc-footer{color:#9aa7b8;text-align:center;padding:28px 0 0;font-size:.9rem}
.spc-footer a{color:#55c7ff;text-decoration:none}
.q{color:#9aa7b8}
small{color:#9aa7b8}
</style>
)HTML";

const char header[] PROGMEM = R"HTML(
<header class="spc-brandbar">
  <a class="spc-brand" href="https://spacepc.dev">SpacePC<span>.dev</span></a>
</header>
<p class="spc-intro">Dieses Gerät wird lokal eingerichtet. Ein Cloud-Konto ist nicht erforderlich.</p>
)HTML";

const char deviceSection[] PROGMEM =
    "<h2>Gerät</h2><small>Aktualisierungsintervall des Displays.</small>";

const char weatherSection[] PROGMEM =
    "<h2>OpenWeatherMap</h2><small>Wetterquelle für aktuelles Wetter und die 5-Tage-Vorschau. "
    "Der API-Key wird im OpenWeatherMap-Konto unter My API keys erstellt.</small>";

const char footer[] PROGMEM =
    "<footer class='spc-footer'>Firmware und Projekte von "
    "<a href='https://spacepc.dev'>SpacePC.dev</a> · "
    "<a href='https://spacepc.dev/impressum/'>Impressum</a></footer>";

}  // namespace PortalTheme
