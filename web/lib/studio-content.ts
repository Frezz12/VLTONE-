export function studioContent(locale: string) {
  const ru = locale === "ru";
  return {
    intro: ru
      ? "VLTone — программа для записи, работы с MIDI и сведения. Записывай вокал и инструменты, создавай аранжировки и экспортируй готовый трек."
      : "VLTone brings recording, MIDI editing and mixing into one app. Record vocals and instruments, build arrangements and export your finished track.",
    start: ru ? "Скачать VLTone" : "Download VLTone",
    explore: ru ? "Открыть инструкцию" : "Read the manual",
    gallery: [
      { name: "showcase-instrumental", label: ru ? "Аранжировка" : "Arrangement", alt: ru ? "Инструментальный проект Night Bloom: аудиодорожки с ударными и MIDI-партии баса, клавиш, арпеджио и мелодии" : "Night Bloom instrumental project: audio drum tracks and MIDI parts for bass, keys, arpeggios and melody" },
      { name: "showcase-midi", label: "Piano Roll", alt: ru ? "Партия Warm Keys из Night Bloom: четыре такта аккордов, длительность нот и сила нажатия" : "Warm Keys from Night Bloom: four bars of chords, note lengths and velocity" },
      { name: "showcase-mix", label: ru ? "Микшер" : "Mixer", alt: ru ? "Микшер с каналами инструментов, посылами на общую обработку и мастер-каналом" : "Mixer with instrument channels, sends to shared effects and a master channel" },
      { name: "showcase-rack", label: ru ? "Рэк" : "Rack", alt: ru ? "Рэк дорожки Warm Keys: сэмплер, цепочка эффектов и посылы рядом с аранжировкой" : "Warm Keys rack: sampler, effect chain and sends alongside the arrangement" },
    ],
    allFeatures: ru ? "Все возможности" : "All features",
    workflow: [
      {
        id: "recording", shot: "showcase-audio",
        title: ru ? "Запись и монтаж аудио" : "Record and edit audio",
        copy: ru ? "Записывай несколько дублей и собирай из них нужную партию. Редактируй клипы на таймлайне, а отдельные звуки — во встроенном аудиоредакторе." : "Record multiple takes and build the part you need. Edit clips on the timeline and work on individual sounds in the built-in audio editor.",
        points: ru ? ["Обрезка, растяжение и плавные переходы.", "Настройка высоты тона и зацикливания.", "Импорт аудиофайлов из браузера программы."] : ["Trim, stretch and fade audio.", "Adjust pitch and loop points.", "Import audio from the built-in browser."],
        link: ru ? "Подробнее о работе с аудио" : "Explore audio editing",
        alt: ru ? "Переход Lift из Night Bloom: стереоволна и параметры звука во встроенном аудиоредакторе" : "Lift transition from Night Bloom: stereo waveform and sound controls in the built-in audio editor",
      },
      {
        id: "midi", shot: "showcase-midi",
        title: ru ? "Мелодии, аккорды и ритм" : "Write melodies, chords and beats",
        copy: ru ? "Рисуй ноты в Piano Roll или записывай с MIDI-клавиатуры. Меняй их длительность и динамику, собирай ритмические партии в редакторе паттернов." : "Draw notes in Piano Roll or record a MIDI keyboard. Adjust timing and velocity, then build rhythmic parts in the pattern editor.",
        points: ru ? ["Мелодии и аккорды в одном редакторе.", "Пошаговое создание барабанных партий.", "Встроенный сэмплер для собственных звуков."] : ["Edit melodies and chords together.", "Sequence drum parts step by step.", "Use your own sounds in the built-in sampler."],
        link: ru ? "Подробнее о MIDI" : "Explore MIDI editing",
        alt: ru ? "Warm Keys из Night Bloom: аккордовая последовательность в Piano Roll и динамика нот" : "Warm Keys from Night Bloom: a chord progression in Piano Roll with note velocity below",
      },
      {
        id: "mixing", shot: "showcase-rack",
        title: ru ? "Весь звук — в одном рэке" : "Your sound, in one rack",
        copy: ru ? "Открой дорожку и посмотри на всю цепочку: от инструмента до последнего эффекта. Настраивай устройства прямо в рэке, меняй их порядок и объединяй обработку в группы. Всё рядом — можно сосредоточиться на звуке." : "Open a track and see the whole chain, from the instrument to the final effect. Adjust devices right in the rack, change their order and group your processing. Everything stays together, so you can focus on the sound.",
        points: ru ? ["Инструмент, мини-модули и эффекты в одном окне.", "Группы устройств, микс обработки и посылы.", "Переключение между рэком и микшером.", "Экспорт готового микса или отдельных дорожек."] : ["Instruments, mini-modules and effects in one view.", "Device groups, wet/dry mix and sends.", "Switch between the rack and the mixer.", "Export a finished mix or individual stems."],
        link: ru ? "Подробнее об обработке и сведении" : "Explore processing and mixing",
        alt: ru ? "Рэк Warm Keys: сэмплер и обработка раскрыты рядом; справа — посыл на общий эффект" : "Warm Keys rack: sampler and processing open side by side, with a send to a shared effect on the right",
      },
    ],
    extras: [
      { title: ru ? "Подключай свои плагины" : "Bring your own plugins", copy: ru ? "Инструменты и эффекты VST, VST3 и CLAP. На macOS также поддерживается Audio Unit." : "VST, VST3 and CLAP instruments and effects, plus Audio Unit on macOS.", href: "/manual#plugin-manager" },
      { title: ru ? "Работай с AI-помощником" : "Work with the AI assistant", copy: ru ? "Задавай вопросы в чате и используй помощника для действий в текущем проекте." : "Ask questions in chat and use the assistant to perform actions in your current project.", href: "/manual#ai-assistant" },
      { title: ru ? "Возвращайся к сохранённой работе" : "Recover your work", copy: ru ? "Сохраняй проекты и используй восстановление, если предыдущая сессия завершилась некорректно." : "Save projects and recover your work after an interrupted session.", href: "/manual#recovery" },
    ],
    betaCopy: ru ? "VLTone доступна в открытой бете для Windows и macOS. Для входа в программу используется тот же аккаунт, что и на сайте." : "VLTone is available in open beta for Windows and macOS. Use the same account in the app and on the website.",
    steps: [
      { title: ru ? "Скачай установщик" : "Download the installer", copy: ru ? "Выбери сборку для своей системы. Рядом с каждой версией есть список изменений." : "Choose a build for your system. Each version comes with a changelog.", link: ru ? "Версии и загрузки" : "Versions & downloads", href: "/releases" },
      { title: ru ? "Создай аккаунт" : "Create an account", copy: ru ? "Укажи почту, никнейм и пароль. Затем войди с этими данными в программу." : "Enter your email, nickname and password, then use them to sign in to the app.", link: ru ? "Регистрация" : "Register", href: "/register" },
      { title: ru ? "Настрой звук" : "Set up audio", copy: ru ? "Выбери аудиоустройство, создай дорожку и добавь первый файл или начни запись." : "Choose your audio device, create a track and import a file or start recording.", link: ru ? "Пошаговое руководство" : "Step-by-step guide", href: "/manual#first-launch" },
    ],
    betaNote: ru ? "Программа развивается. Если что-то работает неправильно, отправь описание проблемы через форму на сайте." : "VLTone is still in development. If something does not work as expected, describe the issue using the report form.",
    faqTitle: ru ? "Перед установкой" : "Before installing",
  };
}
