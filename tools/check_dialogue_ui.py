"""Exercise the shipped Angular bundle using real Chromium and a mock of the in-game CEF event bridge.

python tools/check_dialogue_ui.py /path/to/extracted/SkyrimTogetherReborn/UI
Requires Python Playwright and Chromium. No Skyrim executable or saves are used.
"""
import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import shutil
from threading import Thread

from playwright.sync_api import expect, sync_playwright


class Files(SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass


BRIDGE = """
localStorage.setItem("language", "ru");
localStorage.setItem("party_anchor_offset_y", "3");
const callbacks = new Map();
window.sentCalls = [];
window.skyrimtogether = new Proxy({
  on(name, fn) { callbacks.set(name, [...(callbacks.get(name) ?? []), fn]); },
  off(name, fn) { callbacks.set(name, fn ? (callbacks.get(name) ?? []).filter(x => x !== fn) : []); },
}, { get(obj, key) { return obj[key] ?? ((...args) => window.sentCalls.push([key, ...args])); } });
window.emitGame = (event, ...args) => (callbacks.get(event) ?? []).forEach(fn => fn(...args));
window.hasEvent = name => callbacks.has(name);
"""


def check(ui, executable=None):
    directory = Path(ui).resolve()
    if not (directory / "index.html").is_file():
        raise ValueError("Select the compiled UI directory")
    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(Files, directory=directory))
    Thread(target=server.serve_forever, daemon=True).start()
    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(executable_path=executable or shutil.which("chromium"),
                                                args=["--disable-dev-shm-usage"])
            page = browser.new_page(viewport={"width": 1280, "height": 720})
            errors = []
            page.on("pageerror", lambda error: (errors.append(str(error)), print("Page error:", error, flush=True)))
            page.on("console", lambda msg: print("Console:", msg.text[:400], flush=True) if msg.type == "error" else None)
            page.add_init_script(BRIDGE)
            base_url = f"http://127.0.0.1:{server.server_port}"
            page.route("**/*", lambda route: route.continue_() if route.request.url.startswith(base_url + "/")
                       else route.abort())
            page.goto(base_url, wait_until="domcontentloaded")
            page.wait_for_function("window.hasEvent('partyConversation')")
            def emit(*args):
                page.evaluate("args => window.emitGame(...args)", list(args))
            emit("init")
            emit("enterGame")
            emit("connect")
            emit("setLocalPlayerId", 1)
            emit("setName", "Хост")
            emit("observerSubtitle", "Извозчик", "Куда едем?")
            expect(page.locator(".observer-subtitle")).to_have_text("Извозчик — Куда едем?")
            emit("observerSubtitle", "", "")
            expect(page.locator(".observer-subtitle")).to_have_count(0)
            assert page.evaluate("window.sentCalls.some(x=>x[0]==='travelPreferences' && x[1]===1 && x[2]===1)")
            marker = page.locator(".conversation-marker")
            reply = page.locator(".conversation-reply")
            emit("partyConversation", 3, 50, 50, "Гость", "Пропусти нас.", 2, 1, 1)
            expect(marker).to_have_count(0)
            expect(reply).to_have_text("Пропусти нас.")
            assert abs(float(reply.evaluate("el=>getComputedStyle(el).right.slice(0,-2)")) - 204.8) < 1
            assert float(reply.evaluate("el=>getComputedStyle(el).fontSize.slice(0,-2)")) >= 24
            assert reply.bounding_box()["x"] > 640
            page.evaluate("window.firstReply=document.querySelector('.conversation-reply')")
            emit("partyConversation", 3, 50, 50, "Гость", "Пропусти нас.", 2, 1, 1)
            assert page.evaluate("window.firstReply===document.querySelector('.conversation-reply')")
            emit("partyConversation", 3, 50, 50, "Гость", "Сколько стоит?", 2, 1, 2)
            expect(reply).to_have_text("Сколько стоит?")
            assert reply.evaluate("e=>getComputedStyle(e,'::before').filter.startsWith('blur(')")
            assert reply.evaluate("e=>getComputedStyle(e,'::before').backgroundColor.startsWith('rgba(')")
            assert page.evaluate("window.firstReply!==document.querySelector('.conversation-reply')")
            reply.evaluate("el => el.getAnimations().forEach(a => a.finish())")
            expect(reply).to_have_css("opacity", "0")
            emit("hidePartyConversation")
            expect(reply).to_have_count(0)
            emit("partyConversation", 0, 0, 0, "Гость", "", 2, 2, 0)
            expect(marker).to_be_visible()
            expect(marker.locator(".conversation-bearing")).to_have_count(0)
            emit("hidePartyConversation")
            expect(marker).to_have_count(0)
            emit("activate")
            emit("playerConnected", 2, "Гость", 1, "Вайтран")
            emit("partyInfo", [1, 2], 1)
            expect(page.locator("app-group .group-container")).to_have_css("top", "216px")
            emit("openingMenu", True)
            expect(page.locator("app-group")).to_have_count(0)
            emit("openingMenu", False)
            expect(page.locator("app-group .group-container")).to_have_css("top", "216px")
            member = page.locator("app-group .group-member").filter(has_text="Гость")
            emit("peerRange", 2, False)
            expect(member).to_have_css("opacity", "0.42")
            emit("peerRange", 2, True)
            expect(member).to_have_css("opacity", "1")
            emit("observerMode", True)
            expect(page.locator("app-group")).to_have_count(0)
            expect(page.locator(".dialogue-exit")).to_contain_text("Tab")
            for width, height in ((1920,1080), (2560,1440)):
                page.set_viewport_size({"width": width,"height": height})
                for xpos, edge in ((0,"left"),(50,"right")):
                    emit("partyConversation",3,xpos,8,"Гость","Уговорил.",2,7,1)
                    expect(reply).to_be_attached()
                    page.wait_for_function("([edge,w]) => Math.abs(parseFloat(getComputedStyle(document.querySelector('.conversation-reply'))[edge])-w*.16)<1",arg=[edge,width])
            emit("hidePartyConversation")
            emit("observerMode", False)
            expect(page.locator(".dialogue-exit")).to_have_count(0)
            page.set_viewport_size({"width":1280,"height":720})
            for label, decision in (("Нет", 0), ("Да", 1)):
                emit("offerProgress", 19, "Проверка")
                offer = page.get_by_role("dialog", name="Синхронизировать прогресс с участником группы?")
                expect(offer).to_be_visible()
                offer.get_by_role("button",name=label,exact=True).click()
                assert page.evaluate("d=>window.sentCalls.some(c=>c[0]==='answerProgress'&&c[1]===19&&c[2]===d)",decision)
                emit("hideProgress")
                expect(offer).to_have_count(0)
            for label, decision in (("Нет", 0), ("Да", 1)):
                emit("offerCarriage", 2, 7, "Гость")
                dialog = page.get_by_role("dialog", name="Гость отправляется на повозке. Поехать вместе?")
                expect(dialog).to_be_visible()
                dialog.get_by_role("button", name=label, exact=True).click()
                expect(dialog).to_have_count(0)
                assert page.evaluate("d=>window.sentCalls.some(c=>c[0]==='answerCarriage' && c[1]===2 && c[2]===7 && c[3]===d)", decision)
            page.locator(".app-root-menu button").nth(1).click()
            page.locator("app-player-manager .tab-buttons button").nth(1).click()
            promote = page.get_by_role("button", name="Сделать лидером")
            expect(promote).to_have_count(1)
            promote.click()
            assert page.evaluate("window.sentCalls.some(c => c[0]==='changePartyLeader' && c[1]===2)")
            emit("partyInfo", [1, 2], 2)
            expect(promote).to_have_count(0)
            emit("partyInfo", [1, 2], 1)
            expect(promote).to_have_count(1)
            assert not errors, errors
            browser.close()
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ui")
    parser.add_argument("--chromium")
    options = parser.parse_args()
    check(options.ui, options.chromium)
    print("PASS packaged dialogue subtitles, party HP position, cleanup and Russian leadership handoff")
