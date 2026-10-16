#!/usr/bin/env python3
"""Inject the diagnostic-only export control and gzip a separate service UI.

The computation is separated from the write so that a gate can ask what the
bundle ought to be without producing it. check_diagnostic_integration.py used to
run this script and read back what it had just written, which made a stale
committed file impossible to detect, and running the test suite silently undid
any repair.
"""
from pathlib import Path
import gzip

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "web/index.html"
OUTPUT = ROOT / "src/diagnostic_web_ui_gz.h"

panel = '''</div>
<section id="diagnosticExportPanel" class="section" style="margin:0 0 14px;padding:14px;border:1px solid rgba(94,240,168,.3);border-radius:14px;background:#101a1d">
 <div style="display:flex;flex-wrap:wrap;align-items:center;gap:12px">
  <button class="btn primary" id="diagnosticExport" type="button">Скачать диагностику JSON</button>
  <span id="diagnosticExportStatus" aria-live="polite" style="font-size:13px;color:#b8c9c7">Тестовая сборка · выгрузка истории RPM и расхода</span>
 </div>
 <p class="note" style="margin:10px 0 0">Автоснимки сбоев + кольцевой буфер 90 с (2 Гц), раздельные PID 0B/0C/10/33/5E. Данные только в RAM; перезагрузка их очищает. Используются штатные OBD-запросы, CAN self-test не запускается.</p>
</section>
<section class="box" id="app">'''

script = r'''async function downloadDiagnosticBundle(){
 const button=$('diagnosticExport'),state=$('diagnosticExportStatus');
 button.disabled=true;state.textContent='Собираю статус, временную историю и снимки событий…';
 try{
  const [status,trace]=await Promise.all([api('/api/status'),api('/api/diagnostics/trace')]);
  const savedAt=new Date(),bundle={schema:'h2-gauge-diagnostic-bundle-v1',savedAt:savedAt.toISOString(),firmware:{version:status.version,target:status.target},currentStatus:status,trace};
  const text=JSON.stringify(bundle,null,2),blob=new Blob([text],{type:'application/json'}),url=URL.createObjectURL(blob),link=document.createElement('a');
  link.href=url;link.download='h2-gauge-diagnostic-'+savedAt.toISOString().replace(/[:.]/g,'-')+'.json';document.body.appendChild(link);link.click();link.remove();setTimeout(()=>URL.revokeObjectURL(url),1500);
  state.textContent=`JSON сформирован · ${text.length.toLocaleString()} символов · событий: ${(trace.events||[]).length}`;
 }catch(error){state.textContent='Ошибка выгрузки: '+(error?.message||String(error));}
 finally{button.disabled=false;}
}
$('diagnosticExport').addEventListener('click',downloadDiagnosticBundle);
'''
def build():
    """Return (html, compressed) for the current web/index.html.

    Writes nothing. Callers that only want to compare against the committed
    header use this; the compressed length differs between zlib builds, so
    compare the decompressed HTML rather than these bytes.
    """
    html = SOURCE.read_text(encoding="utf-8")
    home_marker = '</div>\n<section class="box" id="app">'
    script_marker = "function set(id,value){"
    assert html.count(home_marker) == 1, "service home insertion point changed"
    assert html.count(script_marker) == 1, "service JS insertion point changed"
    html = html.replace(home_marker, panel, 1)
    html = html.replace(script_marker, script + script_marker, 1)
    return html, gzip.compress(html.encode("utf-8"), compresslevel=9, mtime=0)


def render(compressed):
    """The header file carrying `compressed`."""
    lines = [
        "#pragma once",
        "#include <Arduino.h>",
        "",
        "static const uint8_t DIAGNOSTIC_INDEX_HTML_GZ[] PROGMEM = {",
    ]
    for start in range(0, len(compressed), 16):
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in compressed[start:start + 16]) + ",")
    lines.extend([
        "};",
        f"static constexpr size_t DIAGNOSTIC_INDEX_HTML_GZ_LEN = {len(compressed)};",
        "",
    ])
    return "\n".join(lines)


def main():
    html, compressed = build()
    OUTPUT.write_text(render(compressed), encoding="ascii")
    print(f"Embedded diagnostic UI: {len(html)} bytes HTML -> "
          f"{len(compressed)} bytes gzip at {OUTPUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
