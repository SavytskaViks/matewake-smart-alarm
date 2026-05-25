const UI_VERSION = "UI v3 (custom phase segments)";
console.log("Loaded:", UI_VERSION);

const MQTT_TOPIC_SETTINGS = "diploma/smart_alarm/001/settings";
const MQTT_TOPIC_OPTIMIZED = "diploma/smart_alarm/001/optimized";
const MQTT_TOPIC_ENVIRONMENT = "diploma/smart_alarm/001/environment";
const BROKER_URL = "wss://broker.hivemq.com:8884/mqtt";

let currentJsonData = null;
let optimizedData = null;
let environmentData = null;
let wakeAlertPlayed = false;

function saveSettings() {
  localStorage.setItem("sleepTime", document.getElementById("sleepTime").value);
  localStorage.setItem("wakeStart", document.getElementById("wakeStart").value);
  localStorage.setItem("wakeEnd", document.getElementById("wakeEnd").value);
}

function loadSettings() {
  const sleepTime = localStorage.getItem("sleepTime");
  const wakeStart = localStorage.getItem("wakeStart");
  const wakeEnd = localStorage.getItem("wakeEnd");
  if (sleepTime) document.getElementById("sleepTime").value = sleepTime;
  if (wakeStart) document.getElementById("wakeStart").value = wakeStart;
  if (wakeEnd) document.getElementById("wakeEnd").value = wakeEnd;
}

loadSettings();

const client = mqtt.connect(BROKER_URL);

client.on("connect", () => {
  const status = document.getElementById("connectionStatus");
  status.innerText = " Підключення готове ";
  status.style.color = "#03dac6";

  client.subscribe(MQTT_TOPIC_OPTIMIZED, (err) => {
    if (err) console.error("Subscribe optimized error:", err);
    else console.log("Subscribed to", MQTT_TOPIC_OPTIMIZED);
  });

  client.subscribe(MQTT_TOPIC_ENVIRONMENT, (err) => {
    if (err) console.error("Subscribe environment error:", err);
    else console.log("Subscribed to", MQTT_TOPIC_ENVIRONMENT);
  });
});

client.on("reconnect", () => {
  const status = document.getElementById("connectionStatus");
  status.innerText = "● Перепідключення... — " + UI_VERSION;
  status.style.color = "#bb86fc";
});

client.on("error", (err) => {
  const status = document.getElementById("connectionStatus");
  status.innerText = "● Помилка MQTT: " + err;
  status.style.color = "#ff5252";
});

client.on("message", (topic, message) => {
  try {
    const data = JSON.parse(message.toString());
    if (topic === MQTT_TOPIC_OPTIMIZED) {
      optimizedData = data;

      if (
        data.mode === "WAKE_WINDOW" &&
        data.recommended_now === true &&
        !wakeAlertPlayed
      ) {
        tryPlayWakeAlert();
        wakeAlertPlayed = true;
      }

      if (data.mode !== "WAKE_WINDOW" || data.recommended_now !== true) {
        wakeAlertPlayed = false;
      }
    }

    if (topic === MQTT_TOPIC_ENVIRONMENT) {
      environmentData = data;
    }

    renderDashboard();
  } catch (e) {
    console.error("Помилка парсингу MQTT:", e);
  }
});

function tryPlayWakeAlert() {
  const audio = document.getElementById("wakeSound");
  if (!audio) return;
  audio.currentTime = 0;
  audio.play().catch((err) => console.log("Не вдалося програти звук:", err));
}

function buildRecommendations(env) {
  const tips = [];
  if (!env) return tips;
  if (env.temp != null && env.temp < 16)
    tips.push(
      `Температура занизька (${env.temp.toFixed(0)}°C). Рекомендовано підвищити до 16–20°C.`,
    );
  if (env.temp != null && env.temp > 20)
    tips.push(
      `Температура зависока (${env.temp.toFixed(0)}°C). Варто провітрити кімнату до 16–20°C.`,
    );
  if (env.hum != null && env.hum < 40)
    tips.push(
      `Вологість занизька (${env.hum.toFixed(0)}%). Рекомендується використати зволожувач для підвищення до 40–60%.`,
    );
  if (env.hum != null && env.hum > 60)
    tips.push(
      `Вологість зависока (${env.hum.toFixed(0)}%). Варто провітрити приміщення до 40–60%.`,
    );
  if (env.pressure != null && env.pressure < 980)
    tips.push("Атмосферний тиск знижений. Це може впливати на самопочуття.");
  if (env.pressure != null && env.pressure > 1050)
    tips.push("Атмосферний тиск підвищений. Можливий дискомфорт.");
  if (env.eco2 != null && env.eco2 > 600)
    tips.push("Рівень CO₂ підвищений. Рекомендовано провітрити кімнату.");
  if (env.eco2 != null && env.eco2 < 400)
    tips.push("Дуже низький рівень CO₂. Можлива поломка сенсора.");
  if (env.tvoc != null && env.tvoc > 660)
    tips.push("Рівень TVOC підвищений. Рекомендовано провітрити приміщення.");
  else if (env.tvoc != null && env.tvoc > 220)
    tips.push("Якість повітря середня. Бажано покращити вентиляцію.");
  return tips;
}

function renderDashboard() {
  const opt = optimizedData;
  const env = environmentData;
  const recommendations = buildRecommendations(env);

  let optimizedHtml = "";
  if (opt && (opt.mode === "PRE_WAKE" || opt.mode === "WAKE_WINDOW")) {
    optimizedHtml = `
      <div class="panel">
        <h3 class="panel-title">Оптимізоване пробудження</h3>
        <div class="highlight-time">
          Рекомендований час пробудження:
          <b style="color:#03dac6">${opt.recommended_wake ?? "--:--"}</b>
        </div>
        <div>
          <span class="status-pill">Час на платі: ${opt.ts ?? "--:--"}</span>
          <span class="status-pill">Режим: ${opt.mode ?? "-"}</span>
          <span class="status-pill">Планова фаза: ${opt.base_phase ?? "-"}</span>
          <span class="status-pill">Реальна фаза: ${opt.real_phase ?? "-"}</span>
          <span class="status-pill">Будити зараз: ${opt.recommended_now ? "Так" : "Ні"}</span>
        </div>
      </div>
    `;
  } else {
    optimizedHtml = `
      <div class="panel">
        <h3 class="panel-title">Оптимальне пробудження</h3>
        <p>Оптимізація пробудження стане доступною перед часом підйому</p>
      </div>
    `;
  }

  const environmentHtml = env
    ? `
      <div class="panel">
        <h3 class="panel-title">Стан середовища</h3>
        <div class="metrics">
          <div class="metric">
            <div class="metric-label">Температура</div>
            <div class="metric-value">${env.temp != null ? env.temp.toFixed(0) : "-"} °C</div>
          </div>
          <div class="metric">
            <div class="metric-label">Вологість</div>
            <div class="metric-value">${env.hum != null ? env.hum.toFixed(0) : "-"} %</div>
          </div>
          <div class="metric">
            <div class="metric-label">Тиск</div>
            <div class="metric-value">${env.pressure != null ? env.pressure.toFixed(0) : "-"} hPa</div>
          </div>
          <div class="metric">
            <div class="metric-label">eCO₂</div>
            <div class="metric-value">${env.eco2 ?? "-"} ppm</div>
          </div>
          <div class="metric">
            <div class="metric-label">TVOC</div>
            <div class="metric-value">${env.tvoc ?? "-"} ppb</div>
          </div>
        </div>
        ${
          recommendations.length
            ? `
            <div class="recommendations">
              <div class="recommendations-title">Рекомендації</div>
              <ul>
                ${recommendations.map((tip) => `<li>${tip}</li>`).join("")}
              </ul>
            </div>
          `
            : `
            <div class="recommendations">
              <div class="recommendations-title">Рекомендації для сну</div>
              <div>Показники середовища в межах норми.</div>
            </div>
          `
        }
      </div>
    `
    : `
      <div class="panel">
        <h3 class="panel-title">Стан середовища</h3>
        <p>Очікуються дані про температуру, вологість та якість повітря...</p>
      </div>
    `;
  document.getElementById("optimizedBlock").innerHTML = optimizedHtml;
  document.getElementById("environmentBlock").innerHTML = environmentHtml;
}

const toMin = (t) => {
  const [h, m] = t.split(":").map(Number);
  return h * 60 + m;
};

const fromMin = (min) => {
  let h = Math.floor(min / 60) % 24;
  let m = min % 60;
  return `${String(h).padStart(2, "0")}:${String(m).padStart(2, "0")}`;
};

function normalizeToWindowTimeline(min, bedMin) {
  return min < bedMin ? min + 1440 : min;
}

function calculateAndDisplay() {
  const bedtime = document.getElementById("sleepTime").value;
  const wakeStart = document.getElementById("wakeStart").value;
  const wakeEnd = document.getElementById("wakeEnd").value;

  let bedMin = toMin(bedtime);
  let wsMin = toMin(wakeStart);
  let weMin = toMin(wakeEnd);

  if (wsMin < bedMin) wsMin += 1440;
  if (weMin < bedMin) weMin += 1440;

  const pattern = [
    { phase: "N1", duration: 10, cycle: 1 },
    { phase: "N2", duration: 20, cycle: 1 },
    { phase: "N3", duration: 45, cycle: 1 },
    { phase: "REM", duration: 15, cycle: 1 },

    { phase: "N2", duration: 30, cycle: 2 },
    { phase: "N3", duration: 40, cycle: 2 },
    { phase: "REM", duration: 20, cycle: 2 },

    { phase: "N2", duration: 40, cycle: 3 },
    { phase: "N3", duration: 25, cycle: 3 },
    { phase: "REM", duration: 25, cycle: 3 },

    { phase: "N2", duration: 45, cycle: 4 },
    { phase: "N3", duration: 15, cycle: 4 },
    { phase: "REM", duration: 30, cycle: 4 },

    { phase: "N2", duration: 50, cycle: 5 },
    { phase: "REM", duration: 40, cycle: 5 },

    { phase: "N2", duration: 30, cycle: 6 },
  ];

  const segments = [];
  let current = bedMin;

  for (const item of pattern) {
    const start = current;
    const end = current + item.duration;
    segments.push({
      cycle: item.cycle,
      phase: item.phase,
      start: fromMin(start),
      end: fromMin(end),
      duration_min: item.duration,
    });
    current = end;
  }

  let bestWakeMin = null;
  for (const seg of segments) {
    let segStart = normalizeToWindowTimeline(toMin(seg.start), bedMin);
    let segEnd = normalizeToWindowTimeline(toMin(seg.end), bedMin);
    if (segEnd < segStart) segEnd += 1440;

    const overlapsWindow = segStart < weMin && segEnd > wsMin;

    if ((seg.phase === "N1" || seg.phase === "N2") && overlapsWindow) {
      const entryInWindow = Math.max(segStart, wsMin);
      if (bestWakeMin === null || entryInWindow < bestWakeMin) {
        bestWakeMin = entryInWindow;
      }
    }
  }

  let bestWakeTime = bestWakeMin !== null ? bestWakeMin : weMin;
  currentJsonData = {
    bedtime: bedtime,
    wake_start: wakeStart,
    wake_end: wakeEnd,
    segments: segments,
    recommended_wake: fromMin(bestWakeTime),
  };

  return currentJsonData;
}

function sendToMqtt() {
  const data = calculateAndDisplay();

  if (!data) {
    alert("Не вдалося сформувати графік!");
    return;
  }
  saveSettings();

  const localWakeEl = document.getElementById("localRecommendedWake");
  if (localWakeEl)
    localWakeEl.innerText = `Розрахований час: ${data.recommended_wake}`;

  if (data.wake_start.length !== 5 || data.wake_end.length !== 5) {
    alert("wake_start / wake_end мають бути у форматі HH:MM");
    return;
  }

  const wsMin = toMin(data.wake_start);
  const weMin = toMin(data.wake_end);
  if (weMin <= wsMin) {
    alert("Кінець вікна пробудження має бути пізніше за початок!");
    return;
  }

  const payload = JSON.stringify(data);
  console.log("Publishing to", MQTT_TOPIC_SETTINGS, payload);

  optimizedData = null;
  renderDashboard();

  client.publish(
    MQTT_TOPIC_SETTINGS,
    payload,
    { qos: 1, retain: true },
    (err) => {
      if (!err) alert("Налаштування передано на будильник!");
      else alert("Помилка мережі: " + err);
    },
  );
}
