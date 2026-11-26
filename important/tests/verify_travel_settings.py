import sys
from pathlib import Path
from threading import Thread
from functools import partial
from http.server import ThreadingHTTPServer
from playwright.sync_api import sync_playwright,expect
from check_dialogue_ui import BRIDGE, Files

ui=Path(sys.argv[1])
http=ThreadingHTTPServer(('127.0.0.1',0),partial(Files,directory=ui))
Thread(target=http.serve_forever,daemon=True).start()
base=f'http://127.0.0.1:{http.server_port}'
try:
    with sync_playwright() as p:
        browser=p.chromium.launch(executable_path=next(iter(sys.argv[2:]), None),args=['--disable-dev-shm-usage'])
        page=browser.new_page(viewport={'width':1920,'height':1080})
        page.add_init_script(BRIDGE)
        page.route('**/*',lambda r:r.continue_() if r.request.url.startswith(base+'/') else r.fulfill(status=200,content_type='application/json',body='[]'))
        page.goto(base)
        def event(*args):
            page.evaluate('args=>window.emitGame(...args)',list(args))
        def boot():
            page.wait_for_function("window.hasEvent('offerCarriage')")
            for e in ('init','enterGame','connect','activate'):event(e)
        def flags(a,b):
            assert page.evaluate("([a,b])=>{const c=window.sentCalls.filter(x=>x[0]==='travelPreferences').at(-1);return c?.[1]===a&&c?.[2]===b}",[a,b])
        def settings():
            page.get_by_role('button',name='Настройки',exact=True).click()
            return (page.get_by_label('Совместное быстрое перемещение через NPC',exact=True),page.get_by_label('Совместное быстрое перемещение через маркеры',exact=True))
        boot();npc,marker=settings();expect(npc).to_be_checked();expect(marker).to_be_checked()
        npc.uncheck();flags(0,1);marker.uncheck();flags(0,0);npc.check();flags(1,0)
        page.reload();boot();flags(1,0);npc,marker=settings();expect(npc).to_be_checked();expect(marker).not_to_be_checked()
        marker.check();flags(1,1);page.locator('app-settings .btnCancel').click()
        event('offerCarriage',3,28,'Хост',1)
        dialog=page.get_by_role('dialog',name='Хост переместился по карте. Переместиться следом?')
        expect(dialog).to_be_visible();dialog.get_by_role('button',name='Нет',exact=True).click()
        assert page.evaluate("window.sentCalls.some(c=>c[0]==='answerCarriage'&&c[1]===3&&c[2]===28&&c[3]===0)")
        print('PASS exact Russian settings labels, all four independent combinations, persisted reload, marker invite and decline')
        browser.close()
finally:
    http.shutdown();http.server_close()
