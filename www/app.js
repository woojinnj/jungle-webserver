document.getElementById('echo').addEventListener('click', async () => {
  const res = await fetch('/echo', { method: 'POST', body: 'hello from the browser' });
  document.getElementById('out').textContent = `${res.status}\n${await res.text()}`;
});
