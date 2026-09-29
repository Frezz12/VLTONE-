export function studioContent(locale: string) {
  const ru = locale === "ru";
  return {
    eyebrow: ru ? "VLTone / Программа для создания музыки" : "VLTone / Music production software",
    title: ru ? "Запись, MIDI и сведение." : "Record, arrange and mix.",
    intro: ru ? "Записывайте аудио, редактируйте MIDI, подключайте плагины и экспортируйте готовый микс. Всё в одном проекте." : "Record audio, edit MIDI, use plugins and export your mix. All in one project.",
    start: ru ? "Скачать VLTone" : "Download VLTone",
    explore: ru ? "Открыть инструкцию" : "Read the manual",
    compatibility: ru ? "Форматы и совместимость" : "Formats and compatibility",
    export: ru ? "МИКС И СТЕМЫ" : "MIX & STEMS",
    gallery: [
      { name: "workspace", label: ru ? "Аранжировка" : "Arrangement", alt: ru ? "Рабочее окно VLTone: дорожки, аудиоклипы, дубли и микшер" : "VLTone workspace: tracks, audio clips, takes and mixer" },
      { name: "piano", label: "Piano Roll", alt: ru ? "Piano Roll: MIDI-ноты, их длительность и динамика" : "Piano Roll: MIDI notes, duration and velocity" },
      { name: "mixer", label: ru ? "Микшер" : "Mixer", alt: ru ? "Микшер: уровни, панорама, эффекты, посылы и мастер" : "Mixer: levels, pan, effects, sends and master" },
      { name: "render", label: ru ? "Рендер" : "Render", alt: ru ? "Окно рендера: формат, диапазон, обработка и отдельные дорожки" : "Render window: format, range, processing and stems" },
    ],
    workflowLabel: ru ? "Основные возможности" : "Core features",
    workflowTitle: ru ? "Работа с аудио и MIDI" : "Audio and MIDI editing",
    allFeatures: ru ? "Полный список возможностей" : "All features",
    workflow: [
      { id: "recording", shot: "workspace", title: ru ? "Запись и монтаж" : "Recording & editing", copy: ru ? "Аудиодорожки, несколько дублей, обрезка и растяжение клипов. Импорт файлов из браузера." : "Audio tracks, multiple takes, clip trimming and stretching. Import files from the browser.", link: ru ? "Работа с аудио" : "Audio workflow", alt: ru ? "Аудиоклипы с несколькими дублями на таймлайне" : "Audio clips with multiple takes on the timeline" },
      { id: "midi", shot: "pattern", title: ru ? "Ноты и паттерны" : "Notes & patterns", copy: ru ? "Piano Roll, пошаговый редактор, MIDI-клавиатура и встроенный сэмплер." : "Piano Roll, a step sequencer, MIDI keyboard input and a built-in sampler.", link: ru ? "MIDI и инструменты" : "MIDI & instruments", alt: ru ? "Редактор паттернов с отдельными инструментами и шагами" : "Pattern editor with instrument lanes and steps" },
      { id: "mixing", shot: "mixer", title: ru ? "Сведение и экспорт" : "Mixing & export", copy: ru ? "Эффекты, шины, посылы и автоматизация. Экспорт мастер-микса или отдельных дорожек." : "Effects, buses, sends and automation. Export a master mix or individual stems.", link: ru ? "Микшер и экспорт" : "Mixer & export", alt: ru ? "Каналы микшера с посылами и мастер-каналом" : "Mixer channels with sends and a master channel" },
    ],
    toolsLabel: ru ? "Встроенные инструменты" : "Built-in tools",
    toolsTitle: ru ? "Сэмплер и обработка звука" : "Sampling and effects",
    toolsCopy: ru ? "Основные инструменты доступны в программе. Сторонние плагины подключаются к инструментальным дорожкам и цепочкам эффектов." : "Core tools are included. Add third-party plugins to instrument tracks and effect chains.",
    tools: [
      { shot: "sampler", label: "SAMPLER", title: ru ? "Сэмплы в MIDI-партии" : "Play samples with MIDI", copy: ru ? "Настройте границы, Loop и огибающую. Заменяйте исходный звук, сохраняя параметры инструмента." : "Set sample boundaries, Loop and envelopes. Replace the source sound while keeping the instrument settings.", alt: ru ? "Сэмплер: форма волны, границы воспроизведения и огибающая" : "Sampler: waveform, playback boundaries and envelope", href: "/manual#sampler" },
      { shot: "equalizer", label: "EQUALIZER", title: ru ? "Коррекция частот" : "Frequency control", copy: ru ? "Полосы эквалайзера и общая частотная кривая в одном окне. Точная настройка частоты, усиления и ширины полосы." : "EQ bands and the combined response in one window. Adjust frequency, gain and bandwidth precisely.", alt: ru ? "Эквалайзер с несколькими полосами и частотной кривой" : "Equalizer with multiple bands and a frequency response curve", href: "/manual#vlt-equalizer" },
      { shot: "compressor", label: "COMPRESSOR", title: ru ? "Управление динамикой" : "Dynamics control", copy: ru ? "Режимы Soft и Punch, атака, релиз, Knee и параллельное смешивание. Индикатор показывает подавление усиления." : "Soft and Punch modes, attack, release, knee and parallel mixing. The meter shows gain reduction.", alt: ru ? "Компрессор: кривая сжатия и настройки динамики" : "Compressor: compression curve and dynamics controls", href: "/manual#compressor-delay" },
      { shot: "delay", label: "DELAY", title: ru ? "Стереоповторы" : "Stereo delay", copy: ru ? "Синхронизация с темпом, Ping-Pong, фильтры и семь вариантов окраски повторов." : "Tempo sync, Ping-Pong, filters and seven delay characters.", alt: ru ? "Delay: время, обратная связь, фильтры и характер звучания" : "Delay: timing, feedback, filters and character", href: "/manual#compressor-delay" },
    ],
    openGuide: ru ? "Инструкция" : "Instructions",
    extras: [
      { tag: "AI", title: ru ? "Чат и действия в проекте" : "Chat and project actions", href: "/manual#ai-assistant" },
      { tag: "VST / VST3 / CLAP / AU", title: ru ? "Подключение плагинов" : "Plugin setup", href: "/manual#plugin-manager" },
      { tag: ru ? "ПРОЕКТЫ" : "PROJECTS", title: ru ? "Сохранение и восстановление" : "Saving and recovery", href: "/manual#recovery" },
    ],
    betaLabel: ru ? "Установка" : "Getting started",
    betaTitle: ru ? "Как начать работу" : "Set up VLTone",
    betaCopy: ru ? "Программа находится в открытой бете. Для входа используется тот же аккаунт, что и на сайте." : "VLTone is in open beta. Use the same account in the app and on the website.",
    steps: [
      { title: ru ? "Скачайте установщик" : "Download the installer", copy: ru ? "Выберите доступную сборку для Windows или macOS на странице релизов." : "Choose an available Windows or macOS build on the releases page.", link: ru ? "Версии и загрузки" : "Versions & downloads", href: "/releases" },
      { title: ru ? "Создайте аккаунт" : "Create an account", copy: ru ? "Укажите почту, никнейм и пароль. Затем войдите в программу." : "Enter your email, nickname and password, then sign in to the app.", link: ru ? "Регистрация" : "Register", href: "/register" },
      { title: ru ? "Настройте звук" : "Set up audio", copy: ru ? "Выберите аудиоустройство, создайте дорожку и добавьте первый файл." : "Choose your audio device, create a track and add your first file.", link: ru ? "Пошаговое руководство" : "Step-by-step guide", href: "/manual#first-launch" },
    ],
    betaNote: ru ? "У каждой версии есть список изменений. Об ошибке можно сообщить через форму на сайте." : "Each release includes a changelog. Report issues using the form on this website.",
    faqTitle: ru ? "Перед установкой" : "Before installing",
    cta: ru ? "Установите VLTone" : "Install VLTone",
  };
}
