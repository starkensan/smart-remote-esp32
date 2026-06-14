const device = document.querySelector("#device");
const last = document.querySelector("#last");
const result = document.querySelector("#result");
const send = document.querySelector("#send");

async function refreshStatus() {
  const res = await fetch("/api/status");
  const data = await res.json();
  device.textContent = `${data.device} / ${data.ip} / RSSI ${data.rssi} dBm`;
  last.textContent = JSON.stringify(data.lastDecoded ?? {}, null, 2);
}

send.addEventListener("click", async () => {
  const body = {
    protocol: document.querySelector("#protocol").value,
    value: document.querySelector("#value").value,
    bits: Number(document.querySelector("#bits").value),
  };

  const res = await fetch("/api/send", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  result.textContent = JSON.stringify(await res.json(), null, 2);
  refreshStatus();
});

refreshStatus();
setInterval(refreshStatus, 2000);
