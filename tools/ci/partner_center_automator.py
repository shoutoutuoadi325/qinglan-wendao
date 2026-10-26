#!/usr/bin/env python3
"""
Automates Microsoft Partner Center interactions via Google Chrome AppleScript.
Requires Google Chrome menu: View > Developer > Allow JavaScript from Apple Events.
"""
import subprocess
import sys
import json
import time
import re

def run_chrome_js(js_code: str) -> str:
    escaped_js = js_code.replace('\\', '\\\\').replace('"', '\\"')
    apple_script = f'''
    tell application "Google Chrome"
        tell active tab of front window
            execute javascript "{escaped_js}"
        end tell
    end tell
    '''
    result = subprocess.run(['osascript', '-e', apple_script], capture_output=True, text=True)
    if result.returncode != 0:
        return f"ERROR: {result.stderr.strip()}"
    return result.stdout.strip()

def check_js_permission():
    out = run_chrome_js("1 + 1")
    if "ERROR:" in out:
        if "Allow JavaScript from Apple Events" in out:
            return False, "PERMISSION_DENIED"
        return False, out
    return True, out

def get_tab_info():
    script = '''
    tell application "Google Chrome"
        return {title, URL} of active tab of front window
    end tell
    '''
    res = subprocess.run(['osascript', '-e', script], capture_output=True, text=True)
    return res.stdout.strip()

def set_tab_url(url: str):
    script = f'''
    tell application "Google Chrome"
        set URL of active tab of front window to "{url}"
    end tell
    '''
    subprocess.run(['osascript', '-e', script], capture_output=True, text=True)

def inspect_current_page():
    js = """
    (() => {
        const text = document.body.innerText;
        const buttons = Array.from(document.querySelectorAll('button, a')).map(b => (b.innerText || '').trim()).filter(Boolean);
        return JSON.stringify({
            url: window.location.href,
            title: document.title,
            sampleText: text.substring(0, 500),
            buttons: buttons.slice(0, 30)
        });
    })()
    """
    raw = run_chrome_js(js)
    try:
        return json.loads(raw)
    except Exception:
        return {"raw": raw}

def extract_identities():
    js = """
    (() => {
        const text = document.body.innerText;
        let identityName = '';
        let publisherId = '';
        let storeId = '';

        // Match Package/Identity Name (e.g. 12345Name.Game or alphanumeric)
        const idMatch = text.match(/(?:Package\\s*(?:family)?\\s*name|Identity\\s*name)[\\s:]+([a-zA-Z0-9._-]+)/i);
        if (idMatch) identityName = idMatch[1];

        // Match Publisher ID (CN=...)
        const pubMatch = text.match(/(CN=[a-zA-Z0-9-]+)/i);
        if (pubMatch) publisherId = pubMatch[1];

        // Match Store ID (12 character alphanumeric starting with 9)
        const storeMatch = text.match(/Store\\s*ID[\\s:]+([9][a-zA-Z0-9]{11})/i);
        if (storeMatch) storeId = storeMatch[1];

        return JSON.stringify({
            identityName,
            publisherId,
            storeId,
            fullTextLength: text.length
        });
    })()
    """
    raw = run_chrome_js(js)
    try:
        return json.loads(raw)
    except Exception:
        return {"raw": raw}

if __name__ == "__main__":
    allowed, status = check_js_permission()
    if not allowed:
        print(f"PERMISSION_STATUS: {status}")
        print("Please enable: View -> Developer -> Allow JavaScript from Apple Events in Google Chrome.")
        sys.exit(1)

    print("PERMISSION_STATUS: GRANTED")
    info = inspect_current_page()
    print("PAGE_INSPECT:")
    print(json.dumps(info, indent=2, ensure_ascii=False))
