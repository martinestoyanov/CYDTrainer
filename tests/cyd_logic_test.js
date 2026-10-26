
const assert = require('node:assert/strict');
const nodes = new Map();
function node(id) { if (!nodes.has(id)) nodes.set(id, {value:'0', checked:false,
  textContent:'', style:{}, classList:{add(){},remove(){},toggle(){}},
  addEventListener(){},setAttribute(){},setCustomValidity(){},
  appendChild(){},querySelectorAll(){return []},getAttribute(){return null}});return nodes.get(id); }
global.document={getElementById:node,querySelectorAll:()=>[],createElementNS:()=>node(Math.random()),createElement:()=>node(Math.random())};
global.window=global;global.addEventListener=()=>{};global.requestAnimationFrame=()=>0;
let calls=[];let pending=[];
global.fetch=(url,opts)=>{calls.push(url);return new Promise((resolve,reject)=>{pending.push({resolve,reject,opts});opts.signal.addEventListener('abort',()=>reject(new Error('aborted')));});};

    (function () {
      "use strict";

      /* =========================================================================
       * 1. BUILD EMBEDDED J39200 SEGMENTED LCD GEOMETRY
       * ========================================================================= */
      const digitsLayer = document.getElementById("lcd-digits-layer");
      const barLayer = document.getElementById("lcd-bargraph-layer");

      // 4 Full 7-Segment Digits (Indices 1..4) + Left Leading '1' (Index 0)
      // Positioned to match the Fluke J39200 4-1/2 digit window and allow 100.0 and 5000
      const fullDigitX = [90, 145, 200, 255];

      // Build Digit 0 (Leading '1' with segments B and C)
      const d0Group = document.createElementNS("http://www.w3.org/2000/svg", "g");
      d0Group.setAttribute("transform", "translate(52, 42)");
      d0Group.innerHTML = `
        <polygon id="d0-b" class="lcd-seg" points="16,4 23,10 20,44 14,48 10,44 13,10"/>
        <polygon id="d0-c" class="lcd-seg" points="14,52 20,56 17,90 10,96 7,90 10,56"/>
      `;
      digitsLayer.appendChild(d0Group);

      // Standard bevelled 7-segment polygons (width 42, height 100, slight 3-deg slant)
      const segPolygons = {
        a: "9,2 37,2 41,7 34,13 13,13 6,7",
        b: "38,4 44,9 41,45 35,49 30,45 33,13",
        c: "35,51 40,55 37,91 31,96 26,90 29,55",
        d: "9,87 30,87 35,93 29,98 4,98 0,93",
        e: "4,51 10,55 7,89 1,94 -3,89 0,55",
        f: "7,4 13,12 10,45 4,49 -1,45 2,9",
        g: "7,46 33,46 37,50 32,54 6,54 2,50"
      };

      fullDigitX.forEach((dx, idx) => {
        const dIndex = idx + 1;
        const g = document.createElementNS("http://www.w3.org/2000/svg", "g");
        g.setAttribute("transform", `translate(${dx}, 42)`);
        let inner = "";
        for (const [sKey, pts] of Object.entries(segPolygons)) {
          inner += `<polygon id="d${dIndex}-${sKey}" class="lcd-seg" points="${pts}"/>`;
        }
        g.innerHTML = inner;
        digitsLayer.appendChild(g);

        // Decimal points between digits 1-2, 2-3, 3-4
        if (dIndex < 4) {
          const dp = document.createElementNS("http://www.w3.org/2000/svg", "polygon");
          const dpX = dx + 43;
          dp.setAttribute("id", `dp-${dIndex}`);
          dp.setAttribute("class", "lcd-seg");
          dp.setAttribute("points", `${dpX},131 ${dpX + 7},131 ${dpX + 6},141 ${dpX - 1},141`);
          digitsLayer.appendChild(dp);
        }
      });

      // Build 0–9–0 Bar Graph Scale & 40 Pointer-Topped Segments
      const barStartX = 46;
      const barStep = 7.8; // 40 segments across 312px
      let barSvgHtml = "";

      // 41 scale ticks (0 through 40) with major ticks every 4 units (labeled 0 1 2 3 4 5 6 7 8 9 0)
      for (let i = 0; i <= 40; i++) {
        const tx = (barStartX + i * barStep).toFixed(1);
        const isMajor = (i % 4 === 0);
        const yTop = isMajor ? 176 : 181;
        const strokeW = isMajor ? 1.8 : 1.1;
        barSvgHtml += `<line x1="${tx}" y1="${yTop}" x2="${tx}" y2="187" stroke-width="${strokeW}" class="lcd-static-line"/>`;

        if (isMajor) {
          const labelNum = (i === 40) ? "0" : String(i / 4);
          barSvgHtml += `<text x="${tx}" y="172" font-size="11.5" text-anchor="middle" class="lcd-static-text">${labelNum}</text>`;
        }
      }

      // 40 Active Bar Graph Segments (Indices 1..40) with pointed tops matching J39200 LCD
      for (let s = 1; s <= 40; s++) {
        const sx = (barStartX + (s - 1) * barStep + 1.2).toFixed(1);
        const sw = 5.4;
        const midX = (parseFloat(sx) + sw / 2).toFixed(1);
        const rightX = (parseFloat(sx) + sw).toFixed(1);
        // Pointed top at y=191, shoulders at y=195, base at y=214
        barSvgHtml += `<polygon id="bar-seg-${s}" class="lcd-seg" points="${sx},214 ${sx},195 ${midX},191 ${rightX},195 ${rightX},214"/>`;
      }
      barLayer.innerHTML = barSvgHtml;

      /* =========================================================================
       * 2. SEGMENT CHARACTER MAP & DIFFERENTIAL DOM RENDERER
       * ========================================================================= */
      const CHAR_MAP = {
        " ": [],
        "0": ["a", "b", "c", "d", "e", "f"],
        "1": ["b", "c"],
        "2": ["a", "b", "d", "e", "g"],
        "3": ["a", "b", "c", "d", "g"],
        "4": ["b", "c", "f", "g"],
        "5": ["a", "c", "d", "f", "g"],
        "6": ["a", "c", "d", "e", "f", "g"],
        "7": ["a", "b", "c"],
        "8": ["a", "b", "c", "d", "e", "f", "g"],
        "9": ["a", "b", "c", "d", "f", "g"],
        "-": ["g"],
        "d": ["b", "c", "d", "e", "g"],
        "c": ["d", "e", "g"],
        "C": ["a", "d", "e", "f"],
        "O": ["a", "b", "c", "d", "e", "f"],
        "L": ["d", "e", "f"],
        "E": ["a", "d", "e", "f", "g"],
        "r": ["e", "g"]
      };

      const domCache = new Map();

      function setElemClassOn(id, isOn) {
        const prev = domCache.get(id);
        if (prev === isOn) return;
        const el = document.getElementById(id);
        if (!el) return;
        if (isOn) {
          el.classList.add("on");
        } else {
          el.classList.remove("on");
        }
        domCache.set(id, isOn);
      }

      function setTextContentCached(id, text) {
        const key = "txt:" + id;
        if (domCache.get(key) === text) return;
        const el = document.getElementById(id);
        if (!el) return;
        el.textContent = text;
        domCache.set(key, text);
      }

      function renderLCDMainDigits(leadOne, chars4, dpIndex, minusOn) {
        setElemClassOn("seg-minus", Boolean(minusOn));
        setElemClassOn("d0-b", Boolean(leadOne));
        setElemClassOn("d0-c", Boolean(leadOne));

        for (let i = 1; i <= 4; i++) {
          const ch = chars4[i - 1] || " ";
          const activeSegs = CHAR_MAP[ch] || [];
          ["a", "b", "c", "d", "e", "f", "g"].forEach((s) => {
            setElemClassOn(`d${i}-${s}`, activeSegs.includes(s));
          });
        }

        for (let dp = 1; dp <= 3; dp++) {
          setElemClassOn(`dp-${dp}`, dpIndex === dp);
        }
      }

      function formatValueForLCD(val, decimals, maxVal) {
        if (val === null || val === undefined || Number.isNaN(val)) {
          return { leadOne: false, chars: ["-", "-", "-", "-"], dp: 0, minus: false, overflow: false };
        }
        const minus = val < 0;
        const absVal = Math.abs(val);

        if (absVal > maxVal) {
          return { leadOne: false, chars: [" ", "O", "L", " "], dp: 0, minus: minus, overflow: true };
        }

        const fixedStr = absVal.toFixed(decimals);
        const dotPos = fixedStr.indexOf(".");
        const cleanDigits = fixedStr.replace(".", "");

        let leadOne = false;
        let fourChars = [" ", " ", " ", " "];
        let dpLocation = 0;

        if (cleanDigits.length === 5 && cleanDigits[0] === "1") {
          // e.g., 1000.0 or 100.00 -> uses leading 1
          leadOne = true;
          fourChars = cleanDigits.slice(1).split("");
          if (dotPos > 0) dpLocation = dotPos - 1;
        } else if (cleanDigits.length <= 4) {
          const padLen = 4 - cleanDigits.length;
          const padded = " ".repeat(padLen) + cleanDigits;
          fourChars = padded.split("");
          if (dotPos > 0) {
            dpLocation = dotPos + padLen;
          }
        } else {
          return { leadOne: false, chars: [" ", "O", "L", " "], dp: 0, minus: minus, overflow: true };
        }

        return {
          leadOne: leadOne,
          chars: fourChars,
          dp: dpLocation,
          minus: minus,
          overflow: false
        };
      }

      function renderBarGraph(value, maxRange, isInvalid) {
        if (isInvalid || value === null || value === undefined || Number.isNaN(value)) {
          setElemClassOn("ann-BAR-PLUS", false);
          setElemClassOn("ann-BAR-MINUS", false);
          setElemClassOn("bar-overrange", false);
          for (let s = 1; s <= 40; s++) {
            setElemClassOn(`bar-seg-${s}`, false);
          }
          return;
        }

        setElemClassOn("ann-BAR-PLUS", value >= 0);
        setElemClassOn("ann-BAR-MINUS", value < 0);

        const absVal = Math.abs(value);
        const isOver = absVal > maxRange;
        setElemClassOn("bar-overrange", isOver);

        const ratio = maxRange > 0 ? Math.min(1, Math.max(0, absVal / maxRange)) : 0;
        const litCount = Math.round(ratio * 40);

        for (let s = 1; s <= 40; s++) {
          setElemClassOn(`bar-seg-${s}`, s <= litCount);
        }
      }

      /* =========================================================================
       * 3. APPLICATION STATE & PROVENANCE CALCULATIONS
       * ========================================================================= */
      const state = {
        mode: "duty",          // 'duty' | 'hz' | 'v'
        source: "captured",    // 'captured' | 'commanded' | 'analog'
        vBarRange: 5.0,
        hzBarRange: 10000,
        vLow: 0.50,
        vHigh: 3.60,

        // Connection & Polling state
        demoMode: false,
        demoScenario: "50_calc",
        connState: "DISCONNECTED", // 'LIVE' | 'STALE' | 'DISCONNECTED' | 'DEMO'
        lastTelemetry: null,
        lastSeq: null,
        lastSeqAdvanceTime: 0,
        lastPacketTime: 0,
        pollInFlight: false,
        consecutiveErrors: 0,
        pollTimerId: null,

        // Command Serialization & Throttling state
        cmdInFlight: false,
        queuedDuty: null,
        sliderThrottleTimer: null,
        userDraggingSlider: false,

        // rAF coordination
        needsRender: true
      };

      // CYDTrainer hardening: validate packets before updating the live UI.
      const numericFields = ["seq", "t", "pwm_set", "pwm_duty", "pwm_hz", "pot_v",
        "fluke_v", "pitot_v", "static_v", "rail_v", "pitot_kpa", "static_kpa",
        "qc_kpa", "ias_kt", "alt_ft"];
      const booleanFields = ["pwm_manual", "pwm_avg_sim", "adc_fluke_ok",
        "sensors_sim", "adc_sensors_ok"];
      function validateTelemetry(packet) {
        if (!packet || typeof packet !== "object" || Array.isArray(packet)) {
          throw new Error("Invalid telemetry object");
        }
        for (const key of numericFields) {
          if (typeof packet[key] !== "number" || !Number.isFinite(packet[key])) {
            throw new Error("Invalid telemetry field: " + key);
          }
        }
        for (const key of booleanFields) {
          if (typeof packet[key] !== "boolean") throw new Error("Invalid flag: " + key);
        }
        if (!Number.isInteger(packet.seq) || packet.seq < 0 || packet.t < 0 ||
            packet.pwm_hz < 0 || packet.pwm_set < 0 || packet.pwm_set > 100 ||
            packet.pwm_duty < 0 || packet.pwm_duty > 100) {
          throw new Error("Telemetry outside supported range");
        }
        return packet;
      }
      let pollGeneration = 0;
      let activePollController = null;
      let commandQueue = [];
      let commandGeneration = 0;
      let commandUncertain = false;
      let pendingDutyValue = null;
      function canControl() {
        return !state.demoMode && state.connState === "LIVE" && !commandUncertain;
      }
      function cancelPendingDuty() {
        if (state.sliderThrottleTimer !== null) clearTimeout(state.sliderThrottleTimer);
        state.sliderThrottleTimer = null;
        pendingDutyValue = null;
        commandQueue = commandQueue.filter(command => command.kind !== "duty");
      }
      function invalidatePolling() {
        ++pollGeneration;
        if (activePollController) activePollController.abort();
        if (state.pollTimerId !== null) clearTimeout(state.pollTimerId);
        state.pollTimerId = null;
      }
      const ALL_ANNUNCIATORS = [
        "ann-AUTO", "ann-100ms", "ann-RECORD", "ann-MAX", "ann-MIN", "ann-AVG",
        "ann-H-box", "ann-AC", "ann-DC", "ann-DUTY",
        "ann-u", "ann-m", "ann-V", "ann-A",
        "ann-n", "ann-F", "ann-S", "ann-PCT",
        "ann-M", "ann-k", "ann-OHM", "ann-Hz",
        "ann-BATT-body", "ann-BATT-nub", "ann-BATT-minus", "ann-BATT-plus",
        "ann-BEEP-1", "ann-BEEP-2", "ann-BEEP-3", "ann-BEEP-4",
        "ann-PCT-DELTA", "ann-4000", "ann-mV-bot"
      ];

      function computeExpectedVoltage(dutyPct) {
        if (dutyPct === null || dutyPct === undefined || Number.isNaN(dutyPct)) return null;
        const d = dutyPct / 100.0;
        return state.vLow + d * (state.vHigh - state.vLow);
      }

      function computeVoltageDerivedDuty(vAvg) {
        if (vAvg === null || vAvg === undefined || Number.isNaN(vAvg)) return null;
        const span = state.vHigh - state.vLow;
        if (Math.abs(span) < 1e-6) return null;
        return 100.0 * ((vAvg - state.vLow) / span);
      }

      function getAnalogProvenance(d) {
        if (!d) return "UNAVAILABLE";
        if (d.pwm_avg_sim === true) return "CALCULATED";
        if (d.pwm_avg_sim === false && d.adc_fluke_ok === true) return "ADC MEASURED";
        return "UNAVAILABLE";
      }

      function isNoEdgesDC(d) {
        if (!d) return false;
        return Number.isFinite(d.pwm_hz) && d.pwm_hz < 0.5 &&
          (d.pwm_duty <= 0.05 || d.pwm_duty >= 99.95);
      }

      /* =========================================================================
       * 4. RENDER COORDINATOR (requestAnimationFrame)
       * ========================================================================= */
      function scheduleRender() {
        state.needsRender = true;
      }

      function renderLoop() {
        updateConnectionTimingState();
        if (state.needsRender) {
          state.needsRender = false;
          renderFullUI();
        }
        requestAnimationFrame(renderLoop);
      }

      function updateConnectionTimingState() {
        const now = performance.now();
        if (state.demoMode) {
          const prevState = state.connState;
          if (state.lastTelemetry && (now - state.lastSeqAdvanceTime > 1600)) {
            state.connState = "STALE";
          } else {
            state.connState = "DEMO";
          }
          if (state.connState !== prevState) state.needsRender = true;
        } else {
          const prevState = state.connState;
          if (!state.lastTelemetry || state.consecutiveErrors >= 2) {
            state.connState = "DISCONNECTED";
          } else if ((now - state.lastPacketTime > 1500) || (now - state.lastSeqAdvanceTime > 1500)) {
            state.connState = "STALE";
          } else {
            state.connState = "LIVE";
          }
          if (state.connState !== prevState) state.needsRender = true;
        }

        if (!canControl()) {
          cancelPendingDuty();
          commandQueue = [];
        }
        // Update packet age readout
        if (state.lastPacketTime > 0) {
          const ageMs = Math.max(0, Math.round(now - state.lastPacketTime));
          setTextContentCached("meta-age", `${ageMs} ms`);
        } else {
          setTextContentCached("meta-age", "---");
        }
      }

      function renderFullUI() {
        const d = state.lastTelemetry;
        const isStaleOrDisc = (state.connState === "STALE" || state.connState === "DISCONNECTED");

        // 1. Top Status Bar
        const badgeEl = document.getElementById("conn-badge");
        badgeEl.className = "badge " + (
          state.connState === "LIVE" ? "badge-live" :
          state.connState === "DEMO" ? "badge-demo" :
          state.connState === "STALE" ? "badge-stale" : "badge-disc"
        );
        const connLabel = state.demoMode && state.connState === "STALE"
          ? "DEMO (STALE SEQ)"
          : state.connState;
        setTextContentCached("conn-text", connLabel);

        setTextContentCached("meta-seq", d ? String(d.seq) : "---");
        setTextContentCached("meta-uptime", d ? `${(d.t / 1000).toFixed(1)} s` : "---");

        // 2. Determine Effective Source & Mode Compatibility
        // In Hz mode, Analog source has no frequency signal
        const analogBtn = document.getElementById("src-btn-analog");
        if (state.mode === "hz") {
          analogBtn.disabled = true;
          if (state.source === "analog") {
            state.source = "captured";
          }
        } else {
          analogBtn.disabled = false;
        }

        // Highlight active mode & source buttons
        document.querySelectorAll("[data-mode]").forEach((btn) => {
          btn.classList.toggle("active", btn.getAttribute("data-mode") === state.mode);
        });
        document.querySelectorAll("[data-src]").forEach((btn) => {
          btn.classList.toggle("active", btn.getAttribute("data-src") === state.source);
        });

        // Highlight active provenance box
        document.getElementById("box-src-commanded").classList.toggle("active-focus", state.source === "commanded");
        document.getElementById("box-src-captured").classList.toggle("active-focus", state.source === "captured");
        document.getElementById("box-src-analog").classList.toggle("active-focus", state.source === "analog");

        // 3. Reset all LCD annunciators first, then illuminate ONLY valid active ones
        ALL_ANNUNCIATORS.forEach((id) => setElemClassOn(id, false));

        // Turn on [H] hold box if data is STALE so operator sees frozen state on LCD
        if (state.connState === "STALE") {
          setElemClassOn("ann-H-box", true);
        }

        // 4. Compute LCD Primary Value, Annunciators & Bar Graph
        const anaProv = getAnalogProvenance(d);
        const expV_cmd = d ? computeExpectedVoltage(d.pwm_set) : null;
        const expV_cap = d ? computeExpectedVoltage(d.pwm_duty) : null;
        const estDuty_ana = (d && anaProv !== "UNAVAILABLE") ? computeVoltageDerivedDuty(d.fluke_v) : null;

        let lcdResult = { leadOne: false, chars: ["-", "-", "-", "-"], dp: 0, minus: false };
        let barVal = null;
        let barMax = 100;
        let sourceDesc = "";
        let barScaleDesc = "";

        if (state.mode === "duty") {
          setElemClassOn("ann-DUTY", true);
          setElemClassOn("ann-PCT", true);
          barMax = 100.0;
          barScaleDesc = "0 – 100.0 %";

          if (state.source === "captured") {
            sourceDesc = "CAPTURED CONTROL-PIN (GPIO16)";
            if (d) {
              lcdResult = formatValueForLCD(d.pwm_duty, 1, 199.9);
              barVal = d.pwm_duty;
            }
          } else if (state.source === "commanded") {
            sourceDesc = "COMMANDED TARGET (pwm_set)";
            if (d) {
              lcdResult = formatValueForLCD(d.pwm_set, 1, 199.9);
              barVal = d.pwm_set;
            }
          } else if (state.source === "analog") {
            sourceDesc = `VOLTAGE-DERIVED ESTIMATE (${anaProv})`;
            if (d && estDuty_ana !== null) {
              lcdResult = formatValueForLCD(estDuty_ana, 1, 199.9);
              barVal = estDuty_ana;
            }
          }
        } else if (state.mode === "hz") {
          setElemClassOn("ann-Hz", true);
          barMax = state.hzBarRange;
          barScaleDesc = `0 – ${(state.hzBarRange / 1000).toFixed(0)} kHz`;

          if (state.source === "captured") {
            if (d) {
              if (isNoEdgesDC(d)) {
                sourceDesc = "CAPTURED PIN: NO EDGES / DC";
                setElemClassOn("ann-DC", true);
                lcdResult = { leadOne: false, chars: [" ", "d", "c", " "], dp: 0, minus: false };
                barVal = 0;
              } else {
                sourceDesc = "CAPTURED CONTROL-PIN (GPIO16)";
                if (d.pwm_hz < 0.5) {
                  sourceDesc = "CAPTURED PIN: NO EDGES / CHECK SIGNAL";
                  lcdResult = { leadOne: false, chars: ["-", "-", "-", "-"], dp: 0, minus: false };
                  barVal = null;
                } else {
                  lcdResult = formatValueForLCD(d.pwm_hz, 0, 9999);
                  barVal = d.pwm_hz;
                }
              }
            } else {
              sourceDesc = "CAPTURED CONTROL-PIN (GPIO16)";
            }
          } else if (state.source === "commanded") {
            sourceDesc = "CONFIGURED FIXED OUTPUT (5000 Hz)";
            if (d) {
              lcdResult = formatValueForLCD(5000, 0, 9999);
              barVal = 5000;
            }
          }
        } else if (state.mode === "v") {
          setElemClassOn("ann-DC", true);
          setElemClassOn("ann-V", true);
          barMax = state.vBarRange;
          barScaleDesc = `0 – ${state.vBarRange.toFixed(1)} V`;
          if (state.vBarRange === 4) {
            setElemClassOn("ann-4000", true);
            setElemClassOn("ann-mV-bot", true);
          }

          if (state.source === "analog") {
            sourceDesc = `FLUKE_V (${anaProv})`;
            if (d && anaProv !== "UNAVAILABLE") {
              lcdResult = formatValueForLCD(d.fluke_v, 3, 19.999);
              barVal = d.fluke_v;
            }
          } else if (state.source === "captured") {
            sourceDesc = "EXPECTED V_AVG FROM CAPTURED DUTY";
            if (d && expV_cap !== null) {
              lcdResult = formatValueForLCD(expV_cap, 3, 19.999);
              barVal = expV_cap;
            }
          } else if (state.source === "commanded") {
            sourceDesc = "EXPECTED V_AVG FROM COMMANDED DUTY";
            if (d && expV_cmd !== null) {
              lcdResult = formatValueForLCD(expV_cmd, 3, 19.999);
              barVal = expV_cmd;
            }
          }
        }

        renderLCDMainDigits(lcdResult.leadOne, lcdResult.chars, lcdResult.dp, lcdResult.minus);
        renderBarGraph(barVal, barMax, !d);
        setTextContentCached("lcd-source-desc", sourceDesc);
        setTextContentCached("lcd-bar-scale-desc", barScaleDesc);

        // 5. Populate Three-Source Provenance Cards
        if (d) {
          // Card A: Commanded
          const cmdPill = document.getElementById("pill-cmd-src");
          cmdPill.textContent = d.pwm_manual ? "MANUAL WEB" : "POT KNOB";
          cmdPill.className = "prov-pill " + (d.pwm_manual ? "prov-calc" : "prov-cmd");
          setTextContentCached("val-cmd-duty", `${d.pwm_set.toFixed(1)} %`);
          setTextContentCached("val-cmd-vexp", expV_cmd !== null ? `${expV_cmd.toFixed(3)} V` : "--- V");

          // Card B: Captured Control-Pin Timing
          const capPill = document.getElementById("pill-cap-status");
          const noEdges = isNoEdgesDC(d);
          if (noEdges) {
            capPill.textContent = "NO EDGES / DC";
            capPill.className = "prov-pill prov-calc";
            setTextContentCached("val-cap-hz", "NO EDGES / DC (0 Hz)");
          } else if (d.pwm_hz < 0.5) {
            capPill.textContent = "NO EDGES / CHECK SIGNAL";
            capPill.className = "prov-pill prov-err";
            setTextContentCached("val-cap-hz", "NO EDGES (NOT CONFIRMED DC)");
          } else {
            capPill.textContent = "MCPWM GPIO16";
            capPill.className = "prov-pill prov-ok";
            setTextContentCached("val-cap-hz", `${Math.round(d.pwm_hz)} Hz`);
          }
          setTextContentCached("val-cap-duty", `${d.pwm_duty.toFixed(1)} %`);
          setTextContentCached("val-cap-vexp", expV_cap !== null ? `${expV_cap.toFixed(3)} V` : "--- V");

          // Card C: Analog / Calculated Fallback
          const anaPill = document.getElementById("pill-ana-status");
          anaPill.textContent = anaProv;
          anaPill.className = "prov-pill " + (
            anaProv === "ADC MEASURED" ? "prov-ok" :
            anaProv === "CALCULATED" ? "prov-calc" : "prov-err"
          );

          if (anaProv !== "UNAVAILABLE") {
            setTextContentCached("val-ana-v", `${d.fluke_v.toFixed(3)} V`);
            setTextContentCached("val-ana-est-duty", estDuty_ana !== null ? `${estDuty_ana.toFixed(1)} %` : "--- %");
          } else {
            setTextContentCached("val-ana-v", "UNAVAILABLE");
            setTextContentCached("val-ana-est-duty", "--- %");
          }
          setTextContentCached("val-ana-flags", `sim:${d.pwm_avg_sim ? "1" : "0"} ok:${d.adc_fluke_ok ? "1" : "0"}`);

          // Comparison Deltas
          const diffCapCmd = d.pwm_duty - d.pwm_set;
          setTextContentCached("delta-cap-cmd", `${diffCapCmd >= 0 ? "+" : ""}${diffCapCmd.toFixed(2)} %`);

          if (estDuty_ana !== null) {
            const diffVestCap = estDuty_ana - d.pwm_duty;
            setTextContentCached("delta-vest-cap", `${diffVestCap >= 0 ? "+" : ""}${diffVestCap.toFixed(2)} %`);
          } else {
            setTextContentCached("delta-vest-cap", "N/A");
          }

          if (anaProv !== "UNAVAILABLE" && expV_cap !== null) {
            const diffVVexp = d.fluke_v - expV_cap;
            setTextContentCached("delta-v-vexp", `${diffVVexp >= 0 ? "+" : ""}${diffVVexp.toFixed(3)} V`);
          } else {
            setTextContentCached("delta-v-vexp", "N/A");
          }
        }

        // 6. PWM Controls & Potentiometer Authority
        const controlsDisabled = !canControl();
        document.getElementById("btn-follow-pot").disabled = controlsDisabled;
        document.getElementById("slider-duty").disabled = controlsDisabled;
        document.getElementById("input-duty").disabled = controlsDisabled;
        document.getElementById("btn-set-duty").disabled = controlsDisabled;
        document.querySelectorAll(".btn-preset").forEach((b) => { b.disabled = controlsDisabled; });

        if (d) {
          setTextContentCached("ctrl-auth-text", d.pwm_manual ? "MANUAL WEB OVERRIDE" : "PHYSICAL POTENTIOMETER");
          setTextContentCached("val-pot-v", `${d.pot_v.toFixed(3)} V`);
          const followBtn = document.getElementById("btn-follow-pot");
          followBtn.classList.toggle("pot-active", !d.pwm_manual);

          // Keep slider synced with telemetry pwm_set when operator is not actively dragging
          if (!state.userDraggingSlider && document.activeElement !== document.getElementById("input-duty")) {
            document.getElementById("slider-duty").value = d.pwm_set.toFixed(1);
            document.getElementById("input-duty").value = d.pwm_set.toFixed(1);
          }
        }

        // 7. Secondary Air Data Panel & /zero Gate
        const airPill = document.getElementById("pill-air-status");
        if (!d) {
          airPill.textContent = "NO TELEMETRY";
          airPill.className = "prov-pill prov-err";
        } else if (d.sensors_sim) {
          airPill.textContent = "SIMULATED AIR DATA";
          airPill.className = "prov-pill prov-calc";
        } else if (d.adc_sensors_ok) {
          airPill.textContent = "LIVE ADC SENSORS";
          airPill.className = "prov-pill prov-ok";
        } else {
          airPill.textContent = "SENSOR ADC FAULT";
          airPill.className = "prov-pill prov-err";
        }

        if (d) {
          setTextContentCached("air-ias", `${d.ias_kt.toFixed(1)} kt`);
          setTextContentCached("air-alt", `${Math.round(d.alt_ft)} ft`);
          setTextContentCached("air-qc", `${d.qc_kpa.toFixed(3)} kPa`);
          setTextContentCached("air-rail-v", `${d.rail_v.toFixed(3)} V`);
          setTextContentCached("air-pitot-kpa", `${d.pitot_kpa.toFixed(2)} kPa`);
          setTextContentCached("air-static-kpa", `${d.static_kpa.toFixed(2)} kPa`);
          setTextContentCached("air-pitot-v", `${d.pitot_v.toFixed(3)} V`);
          setTextContentCached("air-static-v", `${d.static_v.toFixed(3)} V`);
        }

        // /zero safety gate: Disabled if in demo mode, stale/disconnected, simulated sensors, or invalid ADC
        const zeroEligible = Boolean(
          canControl() &&
          d &&
          d.sensors_sim === false &&
          d.adc_sensors_ok === true
        );
        const chkZero = document.getElementById("chk-zero-ambient");
        const btnZero = document.getElementById("btn-zero-sensors");
        chkZero.disabled = !zeroEligible;
        if (!zeroEligible) chkZero.checked = false;
        btnZero.disabled = !(zeroEligible && chkZero.checked);
      }

      /* =========================================================================
       * 5. LIVE TELEMETRY POLLING (10 Hz, Single In-Flight, Backoff, Stale Check)
       * ========================================================================= */
      async function pollTelemetryOnce() {
        if (state.demoMode || state.pollInFlight) return;
        state.pollInFlight = true;
        const generation = pollGeneration;
        const controller = new AbortController();
        activePollController = controller;
        const timeoutId = setTimeout(() => controller.abort(), 750);
        try {
          const resp = await fetch("/data", {
            method: "GET", cache: "no-store", signal: controller.signal
          });
          if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
          // Timeout covers the response body as well as the headers.
          const json = validateTelemetry(await resp.json());
          if (generation !== pollGeneration || state.demoMode) return;
          const now = performance.now();
          state.lastPacketTime = now;
          state.consecutiveErrors = 0;
          if (state.lastSeq === null || json.seq !== state.lastSeq) {
            state.lastSeq = json.seq;
            state.lastSeqAdvanceTime = now;
          }
          state.lastTelemetry = json;
          scheduleRender();
        } catch (err) {
          if (generation === pollGeneration && !state.demoMode) {
            state.consecutiveErrors += 1;
            scheduleRender();
          }
        } finally {
          clearTimeout(timeoutId);
          if (activePollController === controller) activePollController = null;
          state.pollInFlight = false;
          scheduleNextPoll();
        }
      }
      function scheduleNextPoll() {
        if (state.pollTimerId) clearTimeout(state.pollTimerId);
        if (state.demoMode) return;

        // 100ms (10 Hz) nominal, exponential backoff up to 2000ms on consecutive errors
        const delay = state.consecutiveErrors === 0
          ? 100
          : Math.min(2000, 100 * Math.pow(1.7, state.consecutiveErrors));

        state.pollTimerId = setTimeout(pollTelemetryOnce, delay);
      }

      /* =========================================================================
       * 6. SERIALIZED COMMAND DISPATCH (/pwm and /zero)
       * ========================================================================= */
      function enqueueCommand(command) {
        if (!canControl()) return;
        command.generation = commandGeneration;
        commandQueue.push(command);
        void drainCommands();
      }
      async function drainCommands() {
        if (state.cmdInFlight) return;
        state.cmdInFlight = true;
        try {
          while (commandQueue.length && canControl()) {
            const command = commandQueue.shift();
            if (command.generation !== commandGeneration) continue;
            if (command.kind === "zero") {
              const d = state.lastTelemetry;
              if (!d || d.sensors_sim || !d.adc_sensors_ok ||
                  !document.getElementById("chk-zero-ambient").checked) continue;
            }
            const controller = new AbortController();
            const timer = setTimeout(() => controller.abort(), 2500);
            setTextContentCached("cmd-flight-status", "Sending " + command.url + "...");
            try {
              const response = await fetch(command.url, {
                method: "GET", cache: "no-store", signal: controller.signal
              });
              const body = (await response.text()).trim();
              if (!response.ok) throw new Error("HTTP " + response.status);
              if (command.kind !== "zero" && body !== "ok") throw new Error("Unexpected reply");
              if (command.generation === commandGeneration && !state.demoMode) {
                if (command.kind === "zero") {
                  const result = JSON.parse(body);
                  if (!Number.isFinite(result.qc_zero_kpa)) throw new Error("Invalid zero reply");
                  document.getElementById("zero-result-msg").textContent =
                    "Saved offset: " + result.qc_zero_kpa.toFixed(4) + " kPa";
                  document.getElementById("chk-zero-ambient").checked = false;
                }
                setTextContentCached("cmd-flight-status", "Accepted; verify returned telemetry");
                scheduleRender();
              }
            } catch (error) {
              // An interrupted GET may already have reached the hardware. Do not
              // retry or dispatch a conflicting command with an unknown outcome.
              commandUncertain = true;
              commandQueue = [];
              cancelPendingDuty();
              setTextContentCached("cmd-flight-status",
                "Command outcome unknown. Check output, then reload before controlling.");
              scheduleRender();
              break;
            } finally {
              clearTimeout(timer);
            }
          }
        } finally {
          state.cmdInFlight = false;
          if (!canControl()) commandQueue = [];
        }
      }
      function sendPwmDutyCommand(dutyVal) {
        if (!canControl() || !Number.isFinite(Number(dutyVal))) return;
        const value = Math.min(100, Math.max(0, Number(dutyVal)));
        // Latest unsent duty replaces older unsent duty, never an active request.
        commandQueue = commandQueue.filter(command => command.kind !== "duty");
        enqueueCommand({ kind: "duty", url: "/pwm?duty=" + value.toFixed(1) });
      }
      function sendFollowPotCommand() {
        if (!canControl()) return;
        cancelPendingDuty();
        commandQueue = commandQueue.filter(command => command.kind !== "pot");
        // Waits behind any active request; no race with manual duty.
        enqueueCommand({ kind: "pot", url: "/pwm?src=pot" });
      }
      function sendZeroSensorsCommand() {
        const d = state.lastTelemetry;
        if (!canControl() || !d || d.sensors_sim || !d.adc_sensors_ok ||
            !document.getElementById("chk-zero-ambient").checked ||
            commandQueue.some(command => command.kind === "zero")) return;
        enqueueCommand({ kind: "zero", url: "/zero" });
      }
      /* =========================================================================
       * 7. ISOLATED OPT-IN DEMO ADAPTER (Never mixes with live telemetry)
       * ========================================================================= */
      let demoIntervalId = null;
      let demoSeqCounter = 1000;
      let demoStartMs = performance.now();

      function buildDemoPacket(scenario) {
        const now = performance.now();
        if (scenario !== "freeze_seq") {
          demoSeqCounter += 1;
        }
        const uptime = Math.round(now - demoStartMs);

        const base = {
          seq: demoSeqCounter,
          t: uptime,
          pwm_set: 50.0,
          pwm_duty: 50.0,
          pwm_hz: 5000,
          pwm_manual: false,
          pot_v: 1.725,
          fluke_v: 2.050,
          pwm_avg_sim: true,
          adc_fluke_ok: false,
          pitot_v: 0.660,
          static_v: 3.310,
          rail_v: 5.012,
          pitot_kpa: 101.32,
          static_kpa: 101.32,
          qc_kpa: 0.000,
          ias_kt: 0.0,
          alt_ft: 112,
          sensors_sim: true,
          adc_sensors_ok: false
        };

        if (scenario === "50_calc") {
          // Default 50% duty with calculated fluke_v fallback
        } else if (scenario === "50_adc") {
          base.pwm_avg_sim = false;
          base.adc_fluke_ok = true;
          base.fluke_v = 2.038; // Slight realistic difference from 2.050V
          base.pwm_duty = 49.9;
        } else if (scenario === "0_dc") {
          base.pwm_set = 0.0;
          base.pwm_duty = 0.0;
          base.pwm_hz = 0; // No edges at 0% DC
          base.pot_v = 0.080;
          base.fluke_v = 0.500;
        } else if (scenario === "100_dc") {
          base.pwm_set = 100.0;
          base.pwm_duty = 100.0;
          base.pwm_hz = 0; // No edges at 100% DC
          base.pwm_manual = true;
          base.fluke_v = 3.600;
        } else if (scenario === "live_air") {
          base.sensors_sim = false;
          base.adc_sensors_ok = true;
          base.pitot_kpa = 103.45;
          base.static_kpa = 101.30;
          base.qc_kpa = 2.150;
          base.ias_kt = 114.6;
          base.alt_ft = 120;
        }

        return base;
      }

      function setDemoMode(enabled) {
        invalidatePolling();
        ++commandGeneration;
        cancelPendingDuty();
        commandQueue = [];
        state.demoMode = enabled;
        state.lastTelemetry = null;
        state.lastSeq = null;
        state.lastPacketTime = 0;
        state.lastSeqAdvanceTime = 0;
        state.consecutiveErrors = 0;
        const btn = document.getElementById("btn-toggle-demo");
        const drawer = document.getElementById("demo-drawer");

        if (enabled) {
          if (state.pollTimerId) clearTimeout(state.pollTimerId);
          btn.textContent = "OPT-IN DEMO MODE: ON";
          btn.classList.add("active");
          drawer.classList.add("visible");

          demoStartMs = performance.now();
          const tickDemo = () => {
            if (!state.demoMode) return;
            const pkt = buildDemoPacket(state.demoScenario);
            const now = performance.now();
            state.lastPacketTime = now;
            if (state.lastSeq === null || pkt.seq !== state.lastSeq) {
              state.lastSeq = pkt.seq;
              state.lastSeqAdvanceTime = now;
            }
            state.lastTelemetry = pkt;
            scheduleRender();
          };
          tickDemo();
          demoIntervalId = setInterval(tickDemo, 100);
        } else {
          if (demoIntervalId) clearInterval(demoIntervalId);
          demoIntervalId = null;
          btn.textContent = "OPT-IN DEMO MODE: OFF";
          btn.classList.remove("active");
          drawer.classList.remove("visible");

          state.lastTelemetry = null;
          state.lastSeq = null;
          state.lastPacketTime = 0;
          state.lastSeqAdvanceTime = 0;
          state.connState = "DISCONNECTED";
          scheduleRender();
          pollTelemetryOnce();
        }
      }

      /* =========================================================================
       * 8. EVENT LISTENERS
       * ========================================================================= */
      // Mode & Source selectors
      document.querySelectorAll("[data-mode]").forEach((btn) => {
        btn.addEventListener("click", () => {
          state.mode = btn.getAttribute("data-mode");
          scheduleRender();
        });
      });

      document.querySelectorAll("[data-src]").forEach((btn) => {
        btn.addEventListener("click", () => {
          if (btn.disabled) return;
          state.source = btn.getAttribute("data-src");
          scheduleRender();
        });
      });

      // Bar graph configurable ranges
      document.getElementById("sel-v-range").addEventListener("change", (e) => {
        state.vBarRange = parseFloat(e.target.value) || 5.0;
        scheduleRender();
      });

      document.getElementById("sel-hz-range").addEventListener("change", (e) => {
        state.hzBarRange = parseFloat(e.target.value) || 10000;
        scheduleRender();
      });

      // V_low / V_high expected terminal voltage tuning bounds
      function updateVoltageBounds() {
        const lowInput = document.getElementById("inp-vlow");
        const highInput = document.getElementById("inp-vhigh");
        const low = lowInput.value.trim() === "" ? NaN : Number(lowInput.value);
        const high = highInput.value.trim() === "" ? NaN : Number(highInput.value);
        const valid = Number.isFinite(low) && Number.isFinite(high) &&
          low >= 0 && low <= 5 && high > low && high <= 15;
        lowInput.setCustomValidity(valid ? "" : "Use 0 <= low < high <= 15 V; low <= 5 V.");
        highInput.setCustomValidity(valid ? "" : "High must exceed low and be <= 15 V.");
        if (valid) { state.vLow = low; state.vHigh = high; scheduleRender(); }
      }
      document.getElementById("inp-vlow").addEventListener("input", updateVoltageBounds);
      document.getElementById("inp-vhigh").addEventListener("input", updateVoltageBounds);
      // Slider & Manual Duty Controls
      const sliderEl = document.getElementById("slider-duty");
      const numInputEl = document.getElementById("input-duty");

      sliderEl.addEventListener("pointerdown", () => { state.userDraggingSlider = true; });
      window.addEventListener("pointerup", () => { state.userDraggingSlider = false; });

      sliderEl.addEventListener("input", () => {
        numInputEl.value = parseFloat(sliderEl.value).toFixed(1);
        pendingDutyValue = Number(sliderEl.value);
        if (state.sliderThrottleTimer === null) {
          state.sliderThrottleTimer = setTimeout(() => {
            state.sliderThrottleTimer = null;
            const value = pendingDutyValue;
            pendingDutyValue = null;
            if (value !== null) sendPwmDutyCommand(value);
          }, 120);
        }
      });

      document.getElementById("btn-set-duty").addEventListener("click", () => {
        const val = parseFloat(numInputEl.value);
        if (!Number.isNaN(val)) {
          sliderEl.value = Math.min(100, Math.max(0, val)).toFixed(1);
          cancelPendingDuty();
          sendPwmDutyCommand(val);
        }
      });

      document.querySelectorAll(".btn-preset").forEach((btn) => {
        btn.addEventListener("click", () => {
          const val = parseFloat(btn.getAttribute("data-preset"));
          sliderEl.value = val.toFixed(1);
          numInputEl.value = val.toFixed(1);
          cancelPendingDuty();
          sendPwmDutyCommand(val);
        });
      });

      document.getElementById("btn-follow-pot").addEventListener("click", () => {
        sendFollowPotCommand();
      });

      // Pitot/Static Zero confirmation & button
      document.getElementById("chk-zero-ambient").addEventListener("change", () => {
        scheduleRender();
      });

      document.getElementById("btn-zero-sensors").addEventListener("click", () => {
        sendZeroSensorsCommand();
      });

      // Demo Mode Toggle & Scenarios
      document.getElementById("btn-toggle-demo").addEventListener("click", () => {
        setDemoMode(!state.demoMode);
      });

      document.querySelectorAll(".btn-demo-scen").forEach((btn) => {
        btn.addEventListener("click", () => {
          document.querySelectorAll(".btn-demo-scen").forEach((b) => b.classList.remove("active"));
          btn.classList.add("active");
          state.demoScenario = btn.getAttribute("data-scen");
          if (state.demoScenario !== "freeze_seq") {
            state.lastSeqAdvanceTime = performance.now();
          }
          scheduleRender();
        });
      });

      /* =========================================================================
       * 9. INITIALIZATION
       * ========================================================================= */
      globalThis.testAPI = {state, validateTelemetry, sendPwmDutyCommand,
        sendFollowPotCommand, setDemoMode, pollTelemetryOnce, invalidatePolling,
        getQueue: () => commandQueue, uncertain: () => commandUncertain};
    })();
  
(async()=>{
 const a=testAPI; a.state.connState='LIVE';
 a.sendPwmDutyCommand(20);a.sendPwmDutyCommand(40);a.sendFollowPotCommand();
 assert.deepEqual(calls,['/pwm?duty=20.0']);
 assert.deepEqual(a.getQueue().map(x=>x.kind),['pot']);
 pending.shift().resolve({ok:true,text:async()=> 'ok'});
 await new Promise(r=>setImmediate(r));
 assert.deepEqual(calls,['/pwm?duty=20.0','/pwm?src=pot']);
 pending.shift().resolve({ok:true,text:async()=> 'ok'});
 await new Promise(r=>setImmediate(r));
 console.log('PASS: serialized duty/FOLLOW POT; unsent duty cancelled');
 assert.throws(()=>a.validateTelemetry({seq:1}));
 console.log('PASS: incomplete telemetry rejected');
 const pkt={seq:1,t:1,pwm_set:50,pwm_duty:50,pwm_hz:500,pot_v:1,fluke_v:1,pitot_v:1,static_v:1,rail_v:3.3,pitot_kpa:1,static_kpa:1,qc_kpa:0,ias_kt:0,alt_ft:0,pwm_manual:true,pwm_avg_sim:false,adc_fluke_ok:true,sensors_sim:false,adc_sensors_ok:true};
 assert.equal(a.validateTelemetry(pkt),pkt);
 for(const value of [null,'50',NaN,Infinity]) assert.throws(()=>a.validateTelemetry({...pkt,pwm_duty:value}));
 assert.throws(()=>a.validateTelemetry({...pkt,pwm_duty:101}));
 console.log('PASS: valid telemetry accepted; malformed numbers and ranges rejected');
 const poll=a.pollTelemetryOnce();const old=pending.shift();
 a.setDemoMode(true);
 old.resolve({ok:true,json:async()=>pkt});await poll;
 assert.notEqual(a.state.lastTelemetry.seq,pkt.seq);
 const count=calls.length;a.sendPwmDutyCommand(70);a.sendFollowPotCommand();assert.equal(calls.length,count);
 console.log('PASS: demo rejects late live response and blocks hardware commands');
 a.setDemoMode(false);a.invalidatePolling();await new Promise(r=>setImmediate(r));pending=[];a.state.connState='LIVE';
 a.sendPwmDutyCommand(30);pending.shift().reject(new Error('connection lost'));
 await new Promise(r=>setImmediate(r));assert.equal(a.uncertain(),true);
 const n=calls.length;a.sendPwmDutyCommand(60);assert.equal(calls.length,n);
 console.log('PASS: uncertain command outcome locks further commands');
 a.invalidatePolling();console.log('ALL LOGIC TESTS PASSED');process.exit(0);
})().catch(e=>{console.error(e);process.exit(1);});
