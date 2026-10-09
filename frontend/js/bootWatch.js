/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * Boot watchdog — the page never stays blank because one module went missing.
 *
 * app.js is one static module graph of ~120 files: the browser runs none of it
 * until every file has arrived. A single request that never ends (a connection
 * that stalls on a lossy Wi-Fi, click-waits §8.5) or fails left the page empty
 * for good, with nothing to say why — and app.js's own error screen is part of
 * the graph that never ran.
 *
 * So this one is a plain script, loaded `async` beside the module and owing
 * nothing to it. It watches the files arrive (resource timing) and waits for
 * app.js to say it ran (`window.mwBooted`). When a file fails, or nothing has
 * arrived for STALL_MS, it reloads the page once — a reload opens fresh
 * connections, and the files that did arrive come from the cache. If the page
 * stalls again within RETRY_WINDOW_MS, it shows a message and a button instead
 * of reloading in a loop.
 *
 * A separate file and not an inline <script>: the page's policy is
 * `script-src 'self'`.
 */
(function () {
    'use strict';

    // Long enough for the largest file over a slow tunnel; resource entries only
    // land when a response ends, so this is the longest gap between two files.
    const STALL_MS = 10000;
    const RETRY_WINDOW_MS = 60000;
    const RETRY_KEY = 'mw-boot-retry';

    const TEXT = {
        en: {
            title: 'The page did not finish loading',
            body: 'Part of the app never arrived. This happens on an unsteady network.',
            reload: 'Reload',
        },
        fr: {
            title: 'La page n’a pas fini de charger',
            body: 'Une partie de l’application n’est pas arrivée. Cela arrive sur un réseau instable.',
            reload: 'Recharger',
        },
        zh: {
            title: '页面未能加载完成',
            body: '应用的一部分没有加载到。网络不稳定时可能会出现这种情况。',
            reload: '重新加载',
        },
    };

    function readRetry() {
        try {
            return Number(sessionStorage.getItem(RETRY_KEY)) || 0;
        } catch {
            return 0;
        }
    }

    function writeRetry(value) {
        try {
            if (value) sessionStorage.setItem(RETRY_KEY, String(value));
            else sessionStorage.removeItem(RETRY_KEY);
        } catch {
            // No storage (private mode, blocked): no automatic reload either.
        }
    }

    function text() {
        const lang = (navigator.language || 'en').slice(0, 2).toLowerCase();
        return TEXT[lang] || TEXT.en;
    }

    function showMessage(detail) {
        const main = document.getElementById('main-content');
        if (!main) return;
        const t = text();
        // Same markup as app.js's error screen, so the stylesheets already know it.
        const view = document.createElement('div');
        view.className = 'hosts-view';
        const header = document.createElement('div');
        header.className = 'hosts-header';
        const h2 = document.createElement('h2');
        h2.textContent = t.title;
        header.appendChild(h2);
        const box = document.createElement('div');
        box.className = 'hosts-error';
        const p = document.createElement('p');
        p.textContent = t.body;
        box.appendChild(p);
        if (detail) {
            const hint = document.createElement('p');
            hint.className = 'hint';
            hint.textContent = detail;
            box.appendChild(hint);
        }
        const button = document.createElement('button');
        button.className = 'btn btn-reload';
        button.textContent = t.reload;
        button.addEventListener('click', () => location.reload());
        box.appendChild(button);
        view.appendChild(header);
        view.appendChild(box);
        main.replaceChildren(view);
    }

    let lastProgress = performance.now();
    let failed = '';
    let done = false;

    let observer = null;
    if (typeof PerformanceObserver === 'function') {
        try {
            observer = new PerformanceObserver(() => {
                lastProgress = performance.now();
            });
            observer.observe({ type: 'resource', buffered: true });
        } catch {
            observer = null;
        }
    }

    // A module of the graph that fails fires `error` on app.js's <script>; it
    // does not bubble, so listen in the capture phase. A stylesheet that fails
    // leaves the app usable and is left alone.
    function onError(evt) {
        const el = evt.target;
        if (el && el.tagName === 'SCRIPT') failed = el.src || 'script';
    }
    window.addEventListener('error', onError, true);

    function stop() {
        done = true;
        clearInterval(timer);
        window.removeEventListener('error', onError, true);
        if (observer) observer.disconnect();
    }

    function recover(detail) {
        stop();
        console.warn('[MW] Boot stalled:', detail);
        const now = Date.now();
        const last = readRetry();
        if (!last || now - last > RETRY_WINDOW_MS) {
            writeRetry(now);
            location.reload();
            return;
        }
        showMessage(detail);
    }

    const timer = setInterval(() => {
        if (done) return;
        if (window.mwBooted) {
            stop();
            writeRetry(0);
            return;
        }
        if (failed) {
            recover(`failed: ${failed}`);
        } else if (performance.now() - lastProgress > STALL_MS) {
            recover(observer ? 'no file arrived for 10 s' : 'not started after 10 s');
        }
    }, 1000);
})();
