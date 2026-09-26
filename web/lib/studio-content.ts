export function studioContent(locale: string) {
  const ru = locale === "ru";
  return {
    eyebrow: ru ? "Студия для создания музыки" : "Your music-making workspace",
    title: ru ? "Твой звук." : "Your sound.",
    intro: ru ? "Записывай. Создавай. Своди. Всё, что нужно твоей музыке, — в одной программе." : "Record. Create. Mix. Everything your music needs, in one place.",
    start: ru ? "Начать создавать" : "Start creating",
    explore: ru ? "Заглянуть внутрь" : "Take a closer look",
    compatibility: ru ? "Форматы и совместимость" : "Formats and compatibility",
    export: ru ? "МИКС + ДОРОЖКИ" : "MIX + STEMS",
    gallery: [
      { name: "workspace", label: ru ? "Аранжировка" : "Arrangement", alt: ru ? "Аранжировка VLTone: аудиодорожки, MIDI, дубли и микшер" : "VLTone arrangement with audio tracks, MIDI, takes and mixer" },
      { name: "piano", label: "Piano Roll", alt: ru ? "Piano Roll VLTone с MIDI-партией" : "A MIDI phrase in VLTone Piano Roll" },
      { name: "mixer", label: ru ? "Микшер" : "Mixer", alt: ru ? "Микшер VLTone: каналы, эффекты, посылы и мастер" : "VLTone mixer with channels, effects, sends and master" },
      { name: "pattern", label: ru ? "Ритмы" : "Patterns", alt: ru ? "Пошаговый редактор ритмов VLTone" : "VLTone step sequencer" },
    ],
    workflowLabel: ru ? "От идеи к треку" : "From idea to track",
    workflowTitle: ru ? "Музыка на первом плане." : "Put your music first.",
    allFeatures: ru ? "Все возможности" : "Explore all features",
    workflow: [
      { id: "recording", shot: "workspace", title: ru ? "Поймай момент." : "Capture the moment.", copy: ru ? "Вокал, инструменты и лучшие дубли — прямо в аранжировке." : "Vocals, instruments and your best takes, right in the arrangement.", link: ru ? "Запись и аудио" : "Recording & audio", alt: ru ? "Записанные аудиоклипы и дубли в VLTone" : "Recorded audio clips and takes in VLTone" },
      { id: "midi", shot: "piano", title: ru ? "Найди свой ритм." : "Find your rhythm.", copy: ru ? "Рисуй мелодии, собирай биты и играй с MIDI-клавиатуры." : "Draw melodies, build beats and play your MIDI keyboard.", link: ru ? "MIDI и ритмы" : "MIDI & patterns", alt: ru ? "Ноты и динамика партии в Piano Roll" : "Notes and velocity in Piano Roll" },
      { id: "mixing", shot: "mixer", title: ru ? "Собери свой звук." : "Shape your sound.", copy: ru ? "Баланс, эффекты, автоматизация. И готовый микс на выходе." : "Balance, effects and automation. A finished mix at the end.", link: ru ? "Сведение и экспорт" : "Mixing & export", alt: ru ? "Каналы и мастер микшера VLTone" : "VLTone mixer channels and master" },
    ],
    toolsLabel: ru ? "Внутри студии" : "Inside the studio",
    toolsTitle: ru ? "Детали меняют всё." : "The details make it yours.",
    toolsCopy: ru ? "Встроенные инструменты. Твои любимые плагины. Пространство для экспериментов." : "Built-in tools. Your favourite plugins. Room to experiment.",
    tools: [
      { shot: "sampler", label: "SAMPLER", title: ru ? "Из сэмпла — в инструмент." : "From sample to instrument.", copy: ru ? "Волна, огибающая и характер твоего звука." : "A waveform, an envelope, a sound of your own.", alt: ru ? "Встроенный сэмплер VLTone с волной и огибающей" : "VLTone sampler with waveform and envelope", href: "/manual#sampler" },
      { shot: "equalizer", label: "EQUALIZER", title: ru ? "Услышать каждую грань." : "Hear every detail.", copy: ru ? "Точная работа с частотами в наглядном эквалайзере." : "Shape frequencies with a clear, visual equalizer.", alt: ru ? "Встроенный эквалайзер VLTone с частотной кривой" : "VLTone equalizer with a frequency curve", href: "/manual#vlt-equalizer" },
    ],
    openGuide: ru ? "Как это работает" : "See how it works",
    extras: [
      { tag: "AI", title: ru ? "Помощник прямо в проекте" : "An assistant in your project", href: "/capabilities#ai" },
      { tag: "VST / CLAP / AU", title: ru ? "Место для твоих плагинов" : "A home for your plugins", href: "/capabilities#plugins" },
      { tag: ru ? "ПРОЕКТЫ" : "PROJECTS", title: ru ? "История и восстановление" : "History and recovery", href: "/capabilities#recovery" },
    ],
    betaLabel: ru ? "Открытая бета" : "Open beta",
    betaTitle: ru ? "Три шага. И ты в студии." : "Three steps. You're in.",
    betaCopy: ru ? "Один аккаунт для сайта и программы. Без приглашений." : "One account for the website and app. No invitation needed.",
    steps: [
      { title: ru ? "Создай аккаунт" : "Create an account", copy: ru ? "Почта, никнейм и пароль." : "Email, nickname and password.", link: ru ? "Зарегистрироваться" : "Register", href: "/register" },
      { title: ru ? "Скачай VLTone" : "Download VLTone", copy: ru ? "Версия для твоего компьютера." : "The version for your computer.", link: ru ? "Выбрать загрузку" : "Choose a download", href: "/releases" },
      { title: ru ? "Начни свой проект" : "Start your project", copy: ru ? "Войди в программу с тем же аккаунтом." : "Sign in to the app with the same account.", link: ru ? "Первый запуск" : "First launch", href: "/manual#first-launch" },
    ],
    betaNote: ru ? "Программа развивается. Сохраняй копии проектов и делись обратной связью." : "The app is evolving. Keep project backups and share your feedback.",
    faqTitle: ru ? "Пара ответов перед стартом." : "Before you press play.",
    cta: ru ? "Звучит как ты." : "Sounds like you.",
  };
}
