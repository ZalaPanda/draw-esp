'use strict';

const byId = (id) => document.getElementById(id);
const sizes = [8, 16, 32, 64, 128];
const palettes = {
    pico8: ['#000000', '#1D2B53', '#7E2553', '#008751', '#AB5236', '#5F574F', '#C2C3C7', '#FFF1E8',
        '#FF004D', '#FFA300', '#FFEC27', '#00E436', '#29ADFF', '#83769C', '#FF77A8', '#FFCCAA'],
    gameboy: ['#0F380F', '#306230', '#8BAC0F', '#9BBC0F'],
    nes: ['#000000', '#FCFCFC', '#7C7C7C', '#0000FC', '#3CBCFC', '#A4E4FC', '#940084', '#D800CC',
        '#F878F8', '#A80020', '#F83800', '#F87858', '#AC7C00', '#F8B800', '#005800', '#58D854'],
};
const storageKey = 'pixel-esp-draft-v1';
const canvas = byId('canvas');
canvas.addEventListener('contextmenu', (event) => event.preventDefault());
['gesturestart', 'gesturechange'].forEach((name) => {
    document.querySelector('.app').addEventListener(name, (event) => event.preventDefault(), { passive: false });
});
const context = canvas.getContext('2d');
const pixelCanvas = document.createElement('canvas');
const pixelContext = pixelCanvas.getContext('2d');
let state = {
    size: 32,
    pixels: Array(32 * 32).fill('#000000'),
    palette: 'pico8',
    color: '#FF004D',
    grid: true,
};
let stroke = null;
let saveTimer = null;
let dirty = false;
let refreshPending = false;
let mutationPending = false;
let importPending = false;

const message = (text, kind = '') => {
    byId('message').textContent = text;
    byId('message').className = kind;
};
const saveStatus = (text, failed = false) => {
    byId('save-status').textContent = text;
    byId('save-status').classList.toggle('error', failed);
};
const isColor = (value) => typeof value === 'string' && /^#[0-9A-F]{6}$/i.test(value);
const restore = () => {
    try {
        const raw = localStorage.getItem(storageKey);
        if (raw === null) return;
        const draft = JSON.parse(raw);
        if (!draft || draft.version !== 1 || !sizes.includes(draft.size) ||
            !Array.isArray(draft.pixels) || draft.pixels.length !== draft.size * draft.size ||
            !draft.pixels.every(isColor) || !Object.hasOwn(palettes, draft.palette) ||
            !isColor(draft.color) || typeof draft.grid !== 'boolean') {
            throw new Error('Saved draft is invalid or from an unsupported version.');
        }
        state = {
            size: draft.size,
            pixels: draft.pixels,
            palette: draft.palette,
            color: draft.color,
            grid: draft.grid,
        };
        saveStatus('LOCAL DRAFT / RESTORED');
    } catch (error) {
        saveStatus(`DRAFT NOT RESTORED / ${error.message}`, true);
        message(`Could not restore local drawing: ${error.message}`, 'error');
    }
};
const save = () => {
    clearTimeout(saveTimer);
    saveTimer = null;
    if (!dirty) return;
    try {
        localStorage.setItem(storageKey, JSON.stringify({ version: 1, ...state }));
        dirty = false;
        saveStatus('LOCAL DRAFT / SAVED');
    } catch (error) {
        saveStatus(`NOT SAVED / ${error.message}`, true);
        message(`Local saving failed. Download a PNG to keep your drawing. ${error.message}`, 'error');
    }
};
const scheduleSave = () => {
    dirty = true;
    clearTimeout(saveTimer);
    saveTimer = setTimeout(save, 250);
};

const renderPixels = () => {
    pixelCanvas.width = state.size;
    pixelCanvas.height = state.size;
    const image = pixelContext.createImageData(state.size, state.size);
    state.pixels.forEach((color, index) => {
        const rgb = parseInt(color.slice(1), 16);
        image.data[index * 4] = (rgb >> 16) & 255;
        image.data[index * 4 + 1] = (rgb >> 8) & 255;
        image.data[index * 4 + 2] = rgb & 255;
        image.data[index * 4 + 3] = 255;
    });
    pixelContext.putImageData(image, 0, 0);
};
const render = () => {
    renderPixels();
    context.imageSmoothingEnabled = false;
    context.drawImage(pixelCanvas, 0, 0, canvas.width, canvas.height);
    if (state.grid) {
        context.strokeStyle = 'rgba(180, 180, 200, 0.3)';
        context.lineWidth = 1;
        context.beginPath();
        for (let index = 1; index < state.size; index++) {
            const point = Math.round(index * canvas.width / state.size) + 0.5;
            context.moveTo(point, 0);
            context.lineTo(point, canvas.height);
            context.moveTo(0, point);
            context.lineTo(canvas.width, point);
        }
        context.stroke();
    }
};
const resizeViewport = () => {
    const bounds = byId('workspace').getBoundingClientRect();
    if (!bounds.width || !bounds.height) return;
    const side = Math.max(1, Math.floor(Math.min(bounds.width, bounds.height) - 6));
    if (canvas.width !== side || canvas.height !== side) {
        canvas.width = side;
        canvas.height = side;
    }
    render();
};
const renderPalette = () => {
    byId('swatches').replaceChildren();
    palettes[state.palette].forEach((color) => {
        const button = document.createElement('button');
        button.type = 'button';
        button.className = 'swatch';
        button.classList.toggle('selected', color === state.color);
        button.setAttribute('aria-label', `Draw with ${color}`);
        button.setAttribute('aria-pressed', String(color === state.color));
        button.title = color;
        const sample = document.createElement('canvas');
        sample.width = 32;
        sample.height = 32;
        sample.setAttribute('aria-hidden', 'true');
        const sampleContext = sample.getContext('2d');
        sampleContext.fillStyle = color;
        sampleContext.fillRect(0, 0, 32, 32);
        button.append(sample);
        button.addEventListener('click', () => {
            state.color = color;
            renderPalette();
            scheduleSave();
        });
        byId('swatches').append(button);
    });
    byId('color-value').textContent = state.color;
};
const syncSettings = () => {
    byId('resolution').value = String(state.size);
    byId('palette').value = state.palette;
    byId('grid-button').classList.toggle('active', state.grid);
    byId('grid-button').setAttribute('aria-pressed', String(state.grid));
    renderPalette();
};
const endStroke = () => {
    if (!stroke) return;
    const pointerId = stroke.id;
    stroke = null;
    if (canvas.hasPointerCapture(pointerId)) canvas.releasePointerCapture(pointerId);
    save();
};
const cellAt = (event) => {
    const rect = canvas.getBoundingClientRect();
    return {
        x: Math.max(0, Math.min(state.size - 1, Math.floor((event.clientX - rect.left) * state.size / rect.width))),
        y: Math.max(0, Math.min(state.size - 1, Math.floor((event.clientY - rect.top) * state.size / rect.height))),
    };
};
const paintLine = (from, to) => {
    let x = from.x;
    let y = from.y;
    const dx = Math.abs(to.x - x);
    const dy = -Math.abs(to.y - y);
    const stepX = x < to.x ? 1 : -1;
    const stepY = y < to.y ? 1 : -1;
    let difference = dx + dy;
    while (true) {
        state.pixels[y * state.size + x] = state.color;
        if (x === to.x && y === to.y) break;
        const twice = 2 * difference;
        if (twice >= dy) { difference += dy; x += stepX; }
        if (twice <= dx) { difference += dx; y += stepY; }
    }
    render();
    scheduleSave();
};
canvas.addEventListener('pointerdown', (event) => {
    if (stroke || importPending || event.button !== 0 || !event.isPrimary) return;
    event.preventDefault();
    const point = cellAt(event);
    canvas.setPointerCapture(event.pointerId);
    stroke = { id: event.pointerId, point };
    paintLine(point, point);
});
canvas.addEventListener('pointermove', (event) => {
    if (!stroke || event.pointerId !== stroke.id) return;
    if (event.pointerType === 'mouse' && !(event.buttons & 1)) {
        endStroke();
        return;
    }
    paintLine(stroke.point, cellAt(event));
    stroke.point = cellAt(event);
});
canvas.addEventListener('pointerup', (event) => {
    if (stroke && event.pointerId === stroke.id) {
        paintLine(stroke.point, cellAt(event));
        endStroke();
    }
});
['pointercancel', 'lostpointercapture'].forEach((name) => canvas.addEventListener(name, (event) => {
    if (stroke && event.pointerId === stroke.id) endStroke();
}));
window.addEventListener('blur', endStroke);
byId('grid-button').addEventListener('click', () => {
    state.grid = !state.grid;
    syncSettings();
    render();
    scheduleSave();
});
byId('palette').addEventListener('change', (event) => {
    state.palette = event.target.value;
    state.color = palettes[state.palette][0];
    renderPalette();
    scheduleSave();
});
byId('resolution').addEventListener('change', (event) => {
    endStroke();
    const size = Number(event.target.value);
    const pixels = Array.from({ length: size * size }, (_, index) => {
        const x = Math.floor((index % size) * state.size / size);
        const y = Math.floor(Math.floor(index / size) * state.size / size);
        return state.pixels[y * state.size + x];
    });
    state.size = size;
    state.pixels = pixels;
    syncSettings();
    render();
    scheduleSave();
});
const switchView = (view) => {
    endStroke();
    ['editor', 'setup'].forEach((name) => {
        byId(name).hidden = name !== view;
        byId(`${name}-tab`).classList.toggle('active', name === view);
        byId(`${name}-tab`).setAttribute('aria-pressed', String(name === view));
    });
    byId('editor-actions').hidden = view !== 'editor';
    if (view === 'editor') resizeViewport();
};
byId('editor-tab').addEventListener('click', () => switchView('editor'));
byId('setup-tab').addEventListener('click', () => switchView('setup'));

const editingControls = ['resolution', 'palette', 'grid-button', 'import-button', 'fit'];
const setImportPending = (pending) => {
    importPending = pending;
    editingControls.forEach((id) => { byId(id).disabled = pending; });
    byId('swatches').querySelectorAll('button').forEach((button) => { button.disabled = pending; });
};
const decodeFile = (file) => new Promise((resolve, reject) => {
    const url = URL.createObjectURL(file);
    const image = new Image();
    image.onload = () => {
        URL.revokeObjectURL(url);
        if (!image.naturalWidth || !image.naturalHeight) {
            reject(new Error('Image has no usable pixels.'));
            return;
        }
        resolve(image);
    };
    image.onerror = () => {
        URL.revokeObjectURL(url);
        reject(new Error('This image could not be decoded. Choose a valid JPEG, PNG, or BMP.'));
    };
    image.src = url;
});
const importImage = async (file) => {
    if (!file || importPending) return;
    if (!['image/jpeg', 'image/png', 'image/bmp', 'image/x-ms-bmp'].includes(file.type)) {
        message('Choose a JPEG, PNG, or BMP image.', 'error');
        return;
    }
    if (!window.confirm('Replace your drawing with this image? This cannot be undone.')) return;
    endStroke();
    setImportPending(true);
    message('Importing image...');
    try {
        const image = await decodeFile(file);
        const buffer = document.createElement('canvas');
        buffer.width = state.size;
        buffer.height = state.size;
        const bufferContext = buffer.getContext('2d');
        bufferContext.fillStyle = '#000000';
        bufferContext.fillRect(0, 0, state.size, state.size);
        bufferContext.imageSmoothingEnabled = false;
        let width = state.size;
        let height = state.size;
        const fit = byId('fit').value;
        if (fit !== 'stretch') {
            const scale = (fit === 'cover' ? Math.max : Math.min)(
                state.size / image.naturalWidth, state.size / image.naturalHeight
            );
            width = image.naturalWidth * scale;
            height = image.naturalHeight * scale;
        }
        bufferContext.drawImage(image, (state.size - width) / 2, (state.size - height) / 2, width, height);
        const rgba = bufferContext.getImageData(0, 0, state.size, state.size).data;
        state.pixels = Array.from({ length: state.size * state.size }, (_, index) => {
            const offset = index * 4;
            return `#${[rgba[offset], rgba[offset + 1], rgba[offset + 2]]
                .map((channel) => channel.toString(16).padStart(2, '0')).join('').toUpperCase()}`;
        });
        render();
        scheduleSave();
        message('Image imported. Original colors kept.', 'success');
    } catch (error) {
        message(`Import failed: ${error.message}`, 'error');
    } finally {
        setImportPending(false);
        byId('image').value = '';
    }
};
byId('import-button').addEventListener('click', () => {
    byId('image').value = '';
    byId('image').click();
});
byId('image').addEventListener('change', (event) => importImage(event.target.files[0]));

const exportPng = (size) => {
    renderPixels();
    const output = document.createElement('canvas');
    output.width = size;
    output.height = size;
    const outputContext = output.getContext('2d');
    outputContext.imageSmoothingEnabled = false;
    outputContext.drawImage(pixelCanvas, 0, 0, size, size);
    return new Promise((resolve, reject) => output.toBlob((blob) => {
        if (blob) resolve(blob);
        else reject(new Error('The browser could not create a PNG.'));
    }, 'image/png'));
};
byId('download-button').addEventListener('click', async () => {
    const button = byId('download-button');
    button.disabled = true;
    try {
        const size = state.size;
        const blob = await exportPng(size);
        const link = document.createElement('a');
        const url = URL.createObjectURL(blob);
        link.href = url;
        link.download = `pixel-esp-${size}x${size}.png`;
        document.body.append(link);
        link.click();
        link.remove();
        setTimeout(() => URL.revokeObjectURL(url), 1000);
        message('PNG downloaded at your grid resolution.', 'success');
    } catch (error) {
        message(`Download failed: ${error.message}`, 'error');
    } finally {
        button.disabled = false;
    }
});

const requestJson = async (path, options = {}) => {
    const response = await fetch(path, { ...options, signal: AbortSignal.timeout(20000) });
    const text = await response.text();
    let data;
    try {
        data = JSON.parse(text);
    } catch {
        throw new Error(`HTTP ${response.status}: ${text || 'Empty response from device'}`);
    }
    if (!response.ok || data.success === false) {
        throw new Error(data.error || `HTTP ${response.status}: ${text}`);
    }
    return data;
};
const refresh = async (announce = false) => {
    if (refreshPending) return;
    refreshPending = true;
    byId('refresh-button').disabled = true;
    try {
        const [device, network] = await Promise.all(['/api/device', '/api/network'].map((path) => requestJson(path)));
        byId('device-name').textContent = `${device.name} / ${device.display_width} x ${device.display_height}`;
        byId('sta-status').textContent = network.sta.connected ? `Connected / ${network.sta.rssi} dBm`
            : network.sta.configured ? 'Configured / disconnected' : 'Not configured';
        byId('sta-ip').textContent = network.sta.ip || '--';
        byId('ap-status').textContent = network.ap.enabled ? network.ap.ssid : 'Off';
        byId('ap-ip').textContent = network.ap.enabled ? network.ap.ip : '--';
        byId('connection').textContent = 'ESP ONLINE';
        byId('connection').className = 'connection online';
        if (announce) message('Device status refreshed.', 'success');
    } catch (error) {
        byId('connection').textContent = 'OFFLINE';
        byId('connection').className = 'connection offline';
        byId('sta-status').textContent = 'Unavailable / device offline';
        byId('sta-ip').textContent = '--';
        byId('ap-status').textContent = 'Unavailable';
        byId('ap-ip').textContent = '--';
        if (announce) message(`Status unavailable: ${error.message}. Drawing still works offline.`, 'error');
    } finally {
        refreshPending = false;
        byId('refresh-button').disabled = false;
    }
};
const mutate = async (buttonId, pendingText, operation) => {
    if (mutationPending) {
        message('A device request is already in progress.', 'error');
        return;
    }
    mutationPending = true;
    const label = byId(buttonId).getAttribute('aria-label') || byId(buttonId).textContent;
    ['send-button', 'wifi-button', 'clear-button'].forEach((id) => { byId(id).disabled = true; });
    message(pendingText);
    byId(buttonId).setAttribute('aria-busy', 'true');
    try {
        await operation();
    } catch (error) {
        message(`${label.trim()} failed: ${error.message}`, 'error');
    } finally {
        mutationPending = false;
        byId(buttonId).removeAttribute('aria-busy');
        ['send-button', 'wifi-button', 'clear-button'].forEach((id) => { byId(id).disabled = false; });
    }
};
byId('send-button').addEventListener('click', () => mutate('send-button', 'Sending drawing to ESP...', async () => {
    const blob = await exportPng(480);
    if (blob.size > 1024 * 1024) throw new Error('PNG exceeds the device upload limit of 1 MiB.');
    await requestJson('/api/image?fit=stretch', {
        method: 'POST',
        headers: { 'Content-Type': 'image/png' },
        body: blob,
    });
    message('Drawing sent to the device.', 'success');
}));
byId('clear-button').addEventListener('click', () => {
    if (!window.confirm('Clear the device display? Your local drawing will be kept.')) return;
    mutate('clear-button', 'Clearing device display...', async () => {
        await requestJson('/api/image', { method: 'DELETE' });
        message('Device cleared. Your local drawing is unchanged.', 'success');
    });
});
byId('wifi').addEventListener('submit', (event) => {
    event.preventDefault();
    const values = Object.fromEntries(new FormData(event.currentTarget));
    mutate('wifi-button', 'Saving Wi-Fi settings...', async () => {
        await requestJson('/api/wifi', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(values),
        });
        byId('wifi').elements.password.value = '';
        message('Wi-Fi saved. Connection changes may disconnect this page.', 'success');
        await refresh();
    });
});
byId('refresh-button').addEventListener('click', () => refresh(true));
document.addEventListener('visibilitychange', () => {
    if (document.hidden) { endStroke(); save(); }
    else refresh();
});
window.addEventListener('pagehide', () => { endStroke(); save(); });
restore();
syncSettings();
new ResizeObserver(resizeViewport).observe(byId('workspace'));
resizeViewport();
refresh();
setInterval(() => { if (!document.hidden) refresh(); }, 10000);
