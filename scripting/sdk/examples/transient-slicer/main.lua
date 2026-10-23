-- VLTONE SDK 1 / Lua 5.5
-- Разрезание аудиоклипа на таймлайне по атакам.
--
-- Запуск:
--   ПКМ по аудиоклипу -> этот скрипт -> «Разрезать».
--
-- Разрешения:
--   project.read
--   project.write
--   files.read
--
-- Исходный аудиофайл не изменяется.
-- Диалог выбора файла не используется.
--
-- Анализируется звучание клипа: Warp, растяжение, реверс и циклы.
-- Разрезы применяются к этому же снимку одним Undo.
--
-- Доступ к источнику должен предоставить VLTONE.
-- Скрипт не обходит файловые разрешения хоста.

local WINDOW_ID = "vlt_transient_slicer"
local MAX_CUTS = 1200
local READ_FRAMES = 16384
local LOG10 = math.log(10)

local settings = {
  sensitivity = 60, -- Больше значение — больше найденных атак.
  gapMs = 90,       -- Минимальный интервал между атаками.
  floorDb = -48,    -- Нижний порог уровня, dBFS.
}

local job = nil
local timerId = nil
local applied = false

----------------------------------------------------------------------
-- Вспомогательные функции
----------------------------------------------------------------------

local function status(message)
  vlt.ui.set{
    id = "status",
    text = tostring(message),
  }
end

local function log_error(message)
  pcall(vlt.log, {
    text = "Transient Slicer: " .. tostring(message),
  })
end

local function finite(value)
  return type(value) == "number"
      and value == value
      and value > -math.huge
      and value < math.huge
end

local function number_value(value, minimum, maximum, fallback)
  if type(value) == "table" then
    value = value.value
  end

  value = tonumber(value)

  if not finite(value) then
    value = fallback
  end

  return math.max(minimum, math.min(maximum, value))
end

local function entity_id(value)
  if type(value) == "string" then
    return value
  end

  if type(value) == "table" then
    return entity_id(
      value.id or value.clipId or value.clip or value.trackId
    )
  end

  return nil
end

----------------------------------------------------------------------
-- Клип, из которого вызвана команда
----------------------------------------------------------------------

local function target_clip(context)
  context = type(context) == "table" and context or {}

  local target =
    type(context.target) == "table" and context.target or {}

  local clipId = entity_id(target.clip or target.clipId)
  local trackId = entity_id(target.track or target.trackId)

  if not clipId
      and (target.type == "clip" or context.type == "clip") then
    clipId = entity_id(target.id)
  end

  -- Объект под правым кликом имеет приоритет над выделением.
  if clipId then
    return clipId, trackId
  end

  -- Если конкретной цели нет, допускается один выделенный клип.
  local selection =
    type(context.selection) == "table" and context.selection or {}

  local chosen = selection.clips or selection.clip

  if type(chosen) == "table" and #chosen > 0 then
    if #chosen ~= 1 then
      return nil, nil
    end

    chosen = chosen[1]
  end

  trackId = nil

  if chosen then
    clipId = entity_id(chosen)

    if type(chosen) == "table" then
      trackId = entity_id(chosen.track or chosen.trackId)
    end
  end

  return clipId, trackId
end

-- Важно: запоминаем цель при запуске скрипта.
-- Последующая смена выделения не переключает инструмент.
local launchContext = vlt.context.get()
local launchClipId, launchTrackId = target_clip(launchContext)

local function lookup_clip(snapshot, clipId, trackId)
  for _, track in ipairs(snapshot.tracks or {}) do
    local tid = entity_id(track.id or track.track)

    if not trackId or tid == trackId then
      for _, clip in ipairs(track.clips or {}) do
        if entity_id(clip.id or clip.clip) == clipId then
          return tid, clip
        end
      end
    end
  end

  return nil, nil
end

local function clip_snapshot(clipId, trackId)
  local request = {
    includeEvents = false,
    clip = clipId,
  }

  if trackId then
    request.track = trackId
  end

  local snapshot = vlt.project.get(request)
  local tid, clip = lookup_clip(snapshot, clipId, trackId)

  if clip then
    return snapshot, tid, clip
  end

  error("Выбранный клип отсутствует в снимке проекта", 0)
end

----------------------------------------------------------------------
-- Источник аудио
----------------------------------------------------------------------

local function stop_timer()
  if timerId then
    pcall(vlt.timers.stop, {
      id = timerId,
    })

    timerId = nil
  end
end

local function dispose_job()
  stop_timer()

  if job and job.renderId then
    pcall(vlt.jobs.cancel, {id = job.renderId})
  end
  if job and job.audio then
    pcall(vlt.audio.close, {
      id = job.audio.id,
    })
  end

  job = nil

  for _, id in ipairs({ "sensitivity", "gap", "floor" }) do
    pcall(vlt.ui.set, {
      id = id,
      enabled = true,
    })
  end

  -- После успешной нарезки повторный запуск в этом окне запрещён:
  -- первоначальный клип уже разделён на части.
  pcall(vlt.ui.set, {
    id = "run",
    enabled = not applied,
  })
end

----------------------------------------------------------------------
-- Детектор атак
----------------------------------------------------------------------

local function positive(value)
  if value > 0 then
    return value
  end

  return 0
end

local function db_ratio(a, b)
  return 20 * math.log((a + 1e-7) / (b + 1e-7)) / LOG10
end

-- Анализ небольшого временного окна:
-- 1. RMS-энергия сигнала.
-- 2. Энергия разностей соседних сэмплов.
-- 3. Рост относительно адаптивного фона и предыдущего окна.
local function finish_hop(scan, hopStartFrame)
  local n = scan.hopCount

  if n <= 0 then
    return
  end

  local rms = math.sqrt(scan.energy / n)
  local hi = math.sqrt(scan.highEnergy / n)

  if scan.avgRms == nil then
    scan.avgRms = rms
    scan.avgHi = hi
  else
    local rmsLift = positive(db_ratio(rms, scan.avgRms))
    local hiLift = positive(db_ratio(hi, scan.avgHi))

    local rise = db_ratio(rms, scan.previousRms)
    local hiRise = db_ratio(hi, scan.previousHi)

    local score = rmsLift + 0.50 * hiLift

    if rms >= scan.noiseFloor
        and score >= scan.minScore
        and (rise > 0.7 or hiRise > 1.2)
        and hopStartFrame - scan.firstFrame >= scan.edgeFrames
        and scan.lastFrame - hopStartFrame >= scan.edgeFrames then

      local previous = scan.cuts[#scan.cuts]

      if not previous
          or hopStartFrame - previous.frame >= scan.gapFrames then

        if #scan.cuts >= MAX_CUTS then
          error(
            "Обнаружено больше " .. MAX_CUTS
            .. " атак. Уменьшите чувствительность "
            .. "или увеличьте минимальный интервал",
            0
          )
        end

        scan.cuts[#scan.cuts + 1] = {
          frame = hopStartFrame,
          score = score,
        }
      end
    end

    scan.avgRms =
      scan.avgRms + scan.alpha * (rms - scan.avgRms)

    scan.avgHi =
      scan.avgHi + scan.alpha * (hi - scan.avgHi)
  end

  scan.previousRms = rms
  scan.previousHi = hi

  scan.hopCount = 0
  scan.energy = 0
  scan.highEnergy = 0
end

----------------------------------------------------------------------
-- Применение разрезов
----------------------------------------------------------------------

local function apply_cuts(scan)
  if #scan.cuts == 0 then
    status(
      "Атаки не найдены. "
      .. "Попробуйте повысить чувствительность или снизить порог"
    )
    return
  end

  local cuts = {}
  for _, cut in ipairs(scan.cuts) do
    cuts[#cuts + 1] = scan.startBeat + cut.frame * scan.beatsPerFrame
  end
  -- Снимок хранит время звучания клипа. Хост проверяет его актуальность,
  -- сортирует точки и отсекает слишком короткие фрагменты до изменения проекта.
  local result = vlt.project.sliceAudio{
    snapshot = scan.snapshotId,
    cuts = cuts,
    label = "Разрезать аудиоклип по атакам",
  }
  if result.cuts == 0 then
    status("Нет атак с достаточным расстоянием от краёв и соседних разрезов")
    return
  end

  applied = true

  local function plural(n, one, few, many)
    local a = n % 10
    local b = n % 100

    if b >= 11 and b <= 14 then
      return many
    end

    if a == 1 then
      return one
    end

    if a >= 2 and a <= 4 then
      return few
    end

    return many
  end

  local count = result.cuts

  status(
    "Готово: "
    .. count .. " "
    .. plural(count, "разрез", "разреза", "разрезов")
    .. ", "
    .. (count + 1) .. " "
    .. plural(count + 1, "фрагмент", "фрагмента", "фрагментов")
    .. ". Отмена: Ctrl+Z"
  )
end

----------------------------------------------------------------------
-- Обработка одного PCM-блока
----------------------------------------------------------------------

local start_analysis

local function analyze_block()
  if job.renderId then
    local rendered = vlt.jobs.get{id = job.renderId}
    if not rendered.complete then
      status(string.format("Подготовка звучания клипа: %.0f%%", rendered.progress * 100))
      return false
    end
    if rendered.error and rendered.error ~= "" then error(rendered.error, 0) end
    start_analysis(rendered.result)
  end
  local scan = assert(job, "Нет активного анализа")
  local remaining = scan.lastFrame - scan.cursor

  if remaining > 0 then
    -- Не более 1 МиБ PCM за один запрос.
    local requested = math.min(
      READ_FRAMES,
      math.floor(262144 / scan.channels),
      remaining
    )

    local info, pcm = vlt.audio.read{
      id = scan.audio.id,
      frame = scan.cursor,
      frames = requested,
    }

    if pcm == nil or #pcm % scan.channels ~= 0 then
      error("SDK вернул некорректный PCM-буфер", 0)
    end

    local received = #pcm / scan.channels

    if received <= 0 or received > requested then
      error(
        "Не удалось прочитать корректный блок PCM-данных",
        0
      )
    end

    local blockFrame = scan.cursor

    for f = 0, received - 1 do
      local offset = f * scan.channels
      local energy = 0
      local highEnergy = 0

      -- Энергия считается отдельно по каналам:
      -- противофазные каналы не гасят друг друга.
      for ch = 1, scan.channels do
        local sample = pcm[offset + ch]

        if not finite(sample) then
          error(
            "Аудиоисточник содержит некорректные значения сэмплов",
            0
          )
        end

        local difference = sample - scan.previousSample[ch]
        scan.previousSample[ch] = sample

        energy = energy + sample * sample
        highEnergy = highEnergy + difference * difference
      end

      scan.energy =
        scan.energy + energy / scan.channels

      scan.highEnergy =
        scan.highEnergy + highEnergy / scan.channels

      scan.hopCount = scan.hopCount + 1

      if scan.hopCount == scan.hopFrames then
        local hopStart =
          blockFrame + f + 1 - scan.hopCount

        finish_hop(scan, hopStart)
      end
    end

    scan.cursor = scan.cursor + received

    local percent =
      100 * (scan.cursor - scan.firstFrame)
      / (scan.lastFrame - scan.firstFrame)

    status(string.format(
      "Анализ: %.0f%%. Найдено атак: %d",
      percent,
      #scan.cuts
    ))
  end

  if scan.cursor >= scan.lastFrame then
    if scan.hopCount > 0 then
      finish_hop(scan, scan.cursor - scan.hopCount)
    end

    vlt.audio.close{
      id = scan.audio.id,
    }

    scan.audio = nil

    apply_cuts(scan)
    dispose_job()

    return true
  end

  return false
end

----------------------------------------------------------------------
-- Таймер анализа
----------------------------------------------------------------------

local tick

local function schedule_tick()
  local handle = vlt.timers.start{
    milliseconds = 30,
    callback = tick,
  }

  timerId =
    type(handle) == "table" and handle.id or handle

  if type(timerId) ~= "string" or timerId == "" then
    error("SDK не вернул идентификатор таймера", 0)
  end
end

tick = function()
  stop_timer()

  if not job then
    return
  end

  local ok, problem = pcall(function()
    local completed = analyze_block()

    if not completed and job then
      schedule_tick()
    end
  end)

  if not ok then
    dispose_job()
    status("Ошибка: " .. tostring(problem))
    log_error(problem)
  end
end

----------------------------------------------------------------------
-- Запуск анализа
----------------------------------------------------------------------

start_analysis = function(snapshot)
  local opened = vlt.audio.open{path = snapshot.path}
  -- Сохраняем поток сразу: dispose_job закроет его при любой ошибке.
  job = {audio = opened}
  local sr, channels, frames = opened.sampleRate, opened.channels, opened.frames
  if not finite(sr) or sr <= 0 or not finite(frames) or frames < 2
      or not finite(channels) or channels < 1 then
    error("Снимок клипа не содержит аудио", 0)
  end
  local hopFrames = math.max(64, math.floor(sr * 0.005))
  local edgeFrames = math.ceil(sr * snapshot.minimumSliceSeconds) + 1
  local previousSamples = {}
  for ch = 1, channels do previousSamples[ch] = 0 end
  job = {
    audio = opened,
    snapshotId = snapshot.snapshot,
    startBeat = snapshot.start,
    beatsPerFrame = 1 / (snapshot.secondsPerBeat * sr),
    channels = channels,
    firstFrame = 0, lastFrame = frames, cursor = 0,
    hopFrames = hopFrames, hopCount = 0,
    energy = 0, highEnergy = 0,
    previousSample = previousSamples,
    avgRms = nil, avgHi = nil, previousRms = 0, previousHi = 0,
    alpha = 1 - math.exp(-hopFrames / (sr * 0.085)),
    minScore = 8.5 - settings.sensitivity * 0.075,
    noiseFloor = 10 ^ (settings.floorDb / 20),
    gapFrames = math.max(edgeFrames, math.ceil(sr * settings.gapMs / 1000)),
    edgeFrames = edgeFrames,
    cuts = {},
  }
end

local function begin_scan()
  if job or applied then return end
  if not launchClipId then
    error("Клип не определён. Вызовите скрипт через правую кнопку по аудиоклипу", 0)
  end
  local snapshot, trackId, clip = clip_snapshot(launchClipId, launchTrackId)
  if snapshot.readOnly then error("Проект доступен только для чтения", 0) end
  if clip.kind ~= "audio" then error("Нужен аудиоклип", 0) end
  local render = vlt.audio.renderClip{
    track = trackId, clip = clip.id, revision = snapshot.revision,
  }
  job = {renderId = render.id}
  for _, id in ipairs({"run", "sensitivity", "gap", "floor"}) do
    vlt.ui.set{id = id, enabled = false}
  end
  status("Подготовка звучания клипа. Исходник остаётся без изменений")
  schedule_tick()
end

local function run_safe()
  local ok, problem = pcall(begin_scan)

  if not ok then
    dispose_job()
    status("Ошибка: " .. tostring(problem))
    log_error(problem)
  end
end

----------------------------------------------------------------------
-- Окно инструмента
----------------------------------------------------------------------

vlt.ui.window{
  id = WINDOW_ID,
  title = "VLTONE · Разрезать по атакам",
  width = 540,
  height = 310,

  children = {
    {
      type = "column",

      children = {
        {
          type = "label",
          text = "Разрезание аудиоклипа по транзиентам",
        },

        {
          type = "number",
          id = "sensitivity",
          label = "Чувствительность",
          min = 1,
          max = 100,
          value = settings.sensitivity,

          onChange = function(value)
            settings.sensitivity = number_value(
              value, 1, 100, 60
            )
          end,
        },

        {
          type = "number",
          id = "gap",
          label = "Минимальный интервал",
          min = 20,
          max = 1000,
          value = settings.gapMs,
          unit = "ms",

          onChange = function(value)
            settings.gapMs = number_value(
              value, 20, 1000, 90
            )
          end,
        },

        {
          type = "number",
          id = "floor",
          label = "Нижний порог уровня",
          min = -90,
          max = -15,
          value = settings.floorDb,
          unit = "dBFS",

          onChange = function(value)
            settings.floorDb = number_value(
              value, -90, -15, -48
            )
          end,
        },

        {
          type = "row",

          children = {
            {
              type = "button",
              id = "run",
              text = "Разрезать",
              onClick = run_safe,
            },

            {
              type = "button",
              text = "Остановить",

              onClick = function()
                if job then
                  dispose_job()

                  status(
                    "Анализ остановлен. Клип не изменён"
                  )
                end
              end,
            },
          },
        },

        {
          type = "label",
          id = "status",

          text = launchClipId
            and (
              "Клип: "
              .. launchClipId
              .. ". Нажмите «Разрезать»."
            )
            or (
              "Закройте окно и вызовите скрипт "
              .. "через ПКМ по аудиоклипу."
            ),
        },
      },
    },
  },
}