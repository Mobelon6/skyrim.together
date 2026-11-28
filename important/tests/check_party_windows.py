"""Drive the two new menus from the compiled CEF bundle, including requests back into the client."""
import json
from pathlib import Path
import shutil,sys
from functools import partial
from http.server import ThreadingHTTPServer
from threading import Thread
sys.path.insert(0,str(Path(__file__).resolve().parent))
from check_dialogue_ui import BRIDGE,Files
from playwright.sync_api import sync_playwright,expect
root=Path(sys.argv[1]).resolve()
with ThreadingHTTPServer(('127.0.0.1',0),partial(Files,directory=root)) as server:
 Thread(target=server.serve_forever,daemon=True).start()
 try:
  with sync_playwright() as p:
   browser=p.chromium.launch(executable_path=shutil.which('chromium') or shutil.which('google-chrome'),args=['--disable-dev-shm-usage'])
   page=browser.new_page(viewport={'width':1920,'height':1080});errors=[]
   page.on('pageerror',lambda x:errors.append(str(x)))
   page.add_init_script(BRIDGE)
   url=f'http://127.0.0.1:{server.server_port}'
   page.route('**/*',lambda r:r.continue_() if r.request.url.startswith(url+'/') else r.abort())
   page.goto(url);page.wait_for_function("window.hasEvent('partyTrade') && window.hasEvent('offerHome')")
   def emit(*args):page.evaluate('a=>window.emitGame(...a)',list(args))
   def last():return page.evaluate('window.sentCalls.at(-1)')
   for event in ('init','enterGame','connect','activate'):emit(event)
   emit('setName','Ведущий');emit('setLocalPlayerId',1)
   emit('partyTrade',17,'Гость',False,'[]','[]')
   expect(page.get_by_role('dialog')).to_contain_text('Гость предлагает вам обмен')
   page.get_by_role('button',name='Да',exact=True).click();assert last()==['tradeAction',17,1,0,1]
   left=[{'id':255,'name':'Стальной меч','count':4}]
   right=[{'id':0xFE000807,'name':'Соль','count':3}]
   emit('partyTrade',17,'Гость',True,json.dumps(left),json.dumps(right))
   box=page.get_by_role('dialog');expect(box).to_contain_text('Общий контейнер')
   box.get_by_role('spinbutton').first.fill('2')
   box.get_by_role('button',name='Положить').click();assert last()==['tradeAction',17,2,255,2]
   box.get_by_role('button',name='Забрать').click();assert last()==['tradeAction',17,3,0xFE000807-(1<<32),1]
   emit('partyTrade',17,'Гость',True,'[]','[]');expect(box).to_contain_text('Контейнер пуст')
   box.get_by_role('button',name='Закрыть').click();assert last()==['tradeAction',17,0,0,1]
   emit('hideTrade');expect(box).to_have_count(0)
   emit('deactivate');emit('openingMenu',True)
   emit('homeChestHud',1,27,5)
   toggle=page.get_by_role('button',name='Сделать личным',exact=True)
   expect(toggle).to_be_visible()
   bounds=toggle.bounding_box();assert abs(bounds['x']/1920-.815)<.003 and abs(bounds['y']/1080-.937)<.003
   toggle.click();assert last()==['tradeAction',27,5,0,1]
   emit('homeChestHud',3,27,6)
   expect(page.get_by_role('dialog')).to_contain_text('Общие предметы будут удалены безвозвратно.')
   page.get_by_role('button',name='Нет',exact=True).click();assert last()==['tradeAction',27,7,0,1]
   page.get_by_role('button',name='Да',exact=True).click();assert last()==['tradeAction',27,6,0,1]
   emit('homeChestHud',4,27,0);expect(page.get_by_role('dialog')).to_have_count(0)
   emit('homeChestHud',2,27,8)
   page.get_by_role('button',name='Сделать общим',exact=True).click();assert last()==['tradeAction',27,8,0,1]
   emit('homeChestHud',0,0,0);expect(toggle).to_have_count(0)
   emit('openingMenu',False);emit('activate')
   # The client supplies complete labels, not bare names. Choices use
   # one-based indexes including owner access actions, not a visitor ID.
   emit('offerHome',22,'В гости к персонажу 202')
   page.get_by_role('button',name='В гости к персонажу 202',exact=True).click();assert last()==['chooseHome',22,1]
   page.get_by_role('button',name='Войти как обычно',exact=True).click();assert last()==['chooseHome',22,0]
   page.get_by_role('button',name='Отмена',exact=True).click();assert last()==['chooseHome',22,-1]
   emit('offerHome',23,'Разрешить посещение\nВ гости к персонажу 303')
   page.get_by_role('button',name='В гости к персонажу 303',exact=True).click();assert last()==['chooseHome',23,2]
   page.get_by_role('button',name='Разрешить посещение',exact=True).click();assert last()==['chooseHome',23,1]
   emit('offerHome',24,'В гости к Гость')
   page.get_by_role('button',name='В гости к Гость',exact=True).click();assert last()==['chooseHome',24,1]
   emit('hideHome');expect(box).to_have_count(0);assert not errors,errors
   browser.close()
 finally:server.shutdown()
print('PASS trade invitation, both inventory panes, quantities, ESL FormID, close, self/visitor/cancel on house door')
