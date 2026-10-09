/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

// A plain script, not a module: run its source as the page would.
const SOURCE = readFileSync(
    join(dirname(fileURLToPath(import.meta.url)), '..', 'js', 'bootWatch.js'),
    'utf8',
);

let observers;
class FakeObserver {
    constructor(cb) {
        this.cb = cb;
        observers.push(this);
    }
    observe() {}
    disconnect() {
        observers = observers.filter((o) => o !== this);
    }
}
const fileArrives = () => observers.forEach((o) => o.cb());

describe('bootWatch', () => {
    let reload;
    beforeEach(() => {
        vi.useFakeTimers({ toFake: ['setInterval', 'clearInterval', 'Date', 'performance'] });
        observers = [];
        reload = vi.fn();
        vi.stubGlobal('PerformanceObserver', FakeObserver);
        vi.stubGlobal('location', { reload });
        sessionStorage.clear();
        delete window.mwBooted;
        document.body.innerHTML = '<main id="main-content"></main>';
        new Function(SOURCE)();
    });
    afterEach(() => {
        vi.unstubAllGlobals();
        vi.useRealTimers();
    });

    it('stands down once app.js ran, and forgets an earlier retry', () => {
        sessionStorage.setItem('mw-boot-retry', String(Date.now()));
        window.mwBooted = true;
        vi.advanceTimersByTime(30000);
        expect(reload).not.toHaveBeenCalled();
        expect(sessionStorage.getItem('mw-boot-retry')).toBeNull();
        expect(observers).toHaveLength(0);
    });

    it('waits while files keep arriving, however long the boot', () => {
        for (let i = 0; i < 6; i++) {
            vi.advanceTimersByTime(8000);
            fileArrives();
        }
        expect(reload).not.toHaveBeenCalled();
    });

    it('reloads once when no file arrived for 10 s', () => {
        vi.advanceTimersByTime(9000);
        expect(reload).not.toHaveBeenCalled();
        vi.advanceTimersByTime(2000);
        expect(reload).toHaveBeenCalledTimes(1);
        expect(sessionStorage.getItem('mw-boot-retry')).not.toBeNull();
        vi.advanceTimersByTime(30000);
        expect(reload).toHaveBeenCalledTimes(1);
    });

    it('reloads at once when a module of the graph fails', () => {
        const script = document.createElement('script');
        script.src = '/js/ui/HostListView.js';
        document.body.appendChild(script);
        script.dispatchEvent(new Event('error'));
        vi.advanceTimersByTime(1000);
        expect(reload).toHaveBeenCalledTimes(1);
    });

    it('says so instead of reloading again after a recent retry', () => {
        sessionStorage.setItem('mw-boot-retry', String(Date.now() - 5000));
        vi.advanceTimersByTime(11000);
        expect(reload).not.toHaveBeenCalled();
        const main = document.getElementById('main-content');
        expect(main.querySelector('.hosts-error')).not.toBeNull();
        main.querySelector('.btn-reload').click();
        expect(reload).toHaveBeenCalledTimes(1);
    });
});
