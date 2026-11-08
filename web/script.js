'use strict';

const byId = (id) => document.getElementById(id);
const message = (text) => {
    byId('message').textContent = text;
};

async function refresh() {
    try {
        const [device, network] = await Promise.all(
            ['/api/device', '/api/network'].map((path) =>
                fetch(path).then((response) => response.json())
            )
        );
        byId('status').textContent = JSON.stringify({ device, network }, null, 2);
    } catch (error) {
        message(error.message);
    }
}

byId('refresh-button').addEventListener('click', refresh);

byId('image').addEventListener('change', (event) => {
    const file = event.target.files[0];
    const preview = byId('preview');
    if (file) {
        preview.src = URL.createObjectURL(file);
        preview.hidden = false;
    }
});

byId('upload-button').addEventListener('click', async () => {
    const file = byId('image').files[0];
    if (!file) {
        message('Choose an image first');
        return;
    }

    const response = await fetch(`/api/image?fit=${byId('fit').value}`, {
        method: 'POST',
        headers: { 'Content-Type': file.type },
        body: file,
    });
    message(await response.text());
});

byId('clear-button').addEventListener('click', async () => {
    const response = await fetch('/api/image', { method: 'DELETE' });
    message(await response.text());
});

byId('wifi').addEventListener('submit', async (event) => {
    event.preventDefault();
    const values = Object.fromEntries(new FormData(event.currentTarget));
    const response = await fetch('/api/wifi', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(values),
    });
    message(await response.text());
    refresh();
});

refresh();
setInterval(refresh, 10000);
