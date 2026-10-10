import { l, type CreatorNode, type DocumentedPort, type Locale, type Localized, type Port, type PortType } from "./types";

const descriptions: Record<string, Localized> = {
  "input.out": l("Входной аудиосигнал канала до обработки этим модулем.", "The channel audio before processing by this module."),
  "output.in": l("Конечный аудиосигнал, который модуль возвращает в канал.", "The final audio that the module returns to the channel."),
  "mix.a": l("Первая аудиоветвь; при Mix = 0 слышна только она.", "First audio branch; Mix = 0 passes this branch alone."),
  "mix.b": l("Вторая аудиоветвь; при Mix = 1 слышна только она.", "Second audio branch; Mix = 1 passes this branch alone."),
  "history.next": l("Значение для записи после вычисления текущего сэмпла. Тип выбирается в свойствах.", "Value to store after the current sample has been evaluated. Its type is selected in node properties."),
  "history.write": l("Разрешение записи Next. Без провода запись включена; выключенный Gate удерживает память.", "Enables writing Next. Writing is on without a wire; an off Gate holds the stored value."),
  "history.reset": l("Включённый Gate сбрасывает память к начальному значению.", "An on Gate resets memory to its initial value."),
  "history.out": l("Сохранённое значение предыдущего сэмпла. Эта задержка разрывает мгновенную обратную связь.", "The value stored from the previous sample. This delay breaks instantaneous feedback."),
  "delay_buffer.in": l("Аудио для записи в кольцевую память после чтения текущего сэмпла.", "Audio written into the ring memory after the current sample is read."),
  "delay_buffer.out": l("Ссылка на память и позицию записи. Подключается к одному или нескольким Read Delay Tap.", "A memory reference and write position. Connect it to one or more Read Delay Tap nodes."),
  "delay_buffer.reset": l("Переход Gate из 0 в 1 очищает историю задержки.", "An off-to-on Gate transition clears the delay history."),
  "delay_read.buffer": l("Память из Delay Buffer, из которой читается задержанный звук.", "Memory from Delay Buffer used to read delayed audio."),
  "biquad.coefficients": l("Array ровно из пяти чисел: b0, b1, b2, a1, a2. Коэффициент a0 уже нормализован к 1.", "An Array of exactly five values: b0, b1, b2, a1, a2. The a0 coefficient is already normalized to 1."),
  "fir.coefficients": l("Непустой Array коэффициентов конечной импульсной характеристики.", "A nonempty Array of finite impulse response coefficients."),
  "biquad_coefficients.out": l("Array из пяти нормализованных коэффициентов для Biquad.", "An Array of five normalized coefficients for Biquad."),
  "table_lookup.table": l("Array значений таблицы. Position выбирает точку между первым и последним элементом.", "An Array of table values. Position selects a point between the first and last element."),
  "sample_hold.gate": l("Переход из выключенного Gate во включённый захватывает Value.", "An off-to-on Gate transition captures Value."),
  "counter.gate": l("Каждый переход Gate из выключенного во включённый увеличивает счётчик на 1.", "Each off-to-on Gate transition increments the counter by 1."),
  "select.gate": l("Выключен — выбрать A; включён — выбрать B.", "Off selects A; on selects B."),
  "audio_select.gate": l("Выключен — выбрать аудио A; включён — выбрать аудио B.", "Off selects audio A; on selects audio B."),
  "context.sample_rate": l("Текущая частота дискретизации в Hz.", "Current sample rate in Hz."),
  "context.time": l("Время текущего контекста обработки в секундах.", "Current processing-context time in seconds."),
  "context.tempo": l("Темп проекта в BPM.", "Project tempo in BPM."),
  "context.beat": l("Позиция контекста обработки в долях.", "Processing-context position in beats."),
  "context.playing": l("Состояние воспроизведения транспорта как Gate.", "Transport playback state as a Gate."),
  "get.valid": l("Включён, если индекс указывает на существующий элемент. Иначе значение результата равно 0.", "On when the index refers to an existing element. Otherwise, the returned value is 0."),
  "stereo_split.left": l("Левый аудиоканал как поток Number.", "The left audio channel as a Number stream."),
  "stereo_split.right": l("Правый аудиоканал как поток Number.", "The right audio channel as a Number stream."),
  "ms_encode.mid": l("Центральная составляющая: (Left + Right) / 2.", "Mid component: (Left + Right) / 2."),
  "ms_encode.side": l("Боковая составляющая: (Left − Right) / 2.", "Side component: (Left − Right) / 2."),
};

const sources: Record<PortType, string> = {
  audio: "Input / Output", number: "Constant / Output", gate: "Compare / Output",
  integer: "Number to Integer / Output", array: "Array / Output", list: "List / Output",
  buffer: "Delay Buffer / Output", function: "C++ Function / function()",
};
const destinations: Record<PortType, string> = {
  audio: "Output / Audio", number: "Smooth / Value", gate: "Select / Select B",
  integer: "Get Element / Index", array: "Get Element / Collection", list: "Length / Collection (List)",
  buffer: "Read Delay Tap / Buffer", function: "C++ Function / callable",
};

export function documentPort(node: CreatorNode, port: Port, direction: "input" | "output", locale: Locale): DocumentedPort {
  const ru = locale === "ru";
  let description = descriptions[`${node.id}.${port.id}`]?.[locale];
  const parameter = node.parameters[port.parameter];
  if (!description && direction === "input" && parameter) {
    description = ru
      ? `Управляет «${parameter.name}» потоком ${port.type}. Без провода используется ручное значение ${parameter.initial}${parameter.unit ? ` ${parameter.unit}` : ""}.`
      : `Controls ${parameter.name} with a ${port.type} stream. Without a wire, the manual value is ${parameter.initial}${parameter.unit ? ` ${parameter.unit}` : ""}.`;
  }
  if (!description && port.id === "reset") {
    description = ["lfo", "oscillator", "random"].includes(node.id)
      ? (ru ? "Переход Gate из 0 в 1 перезапускает генератор." : "An off-to-on Gate transition restarts the generator.")
      : (ru ? "Включённый Gate сбрасывает внутреннее состояние." : "An on Gate resets the internal state.");
  }
  if (!description && port.id === "success") description = ru ? "Включён, если операция с коллекцией выполнена. Проверяй этот выход перед использованием результата." : "On when the collection operation succeeds. Check this output before using the result.";
  if (!description && port.id === "collection") description = ru ? "Исходная коллекция. Выбери соответствующий Array или List в свойствах ноды." : "Source collection. Select the matching Array or List type in node properties.";
  if (!description && port.id === "index") description = ru ? "Целочисленный индекс элемента, начиная с 0. Для Number нужен Number to Integer." : "Zero-based integer element index. Convert a Number using Number to Integer.";
  if (!description && direction === "input") description = ru
    ? `${port.name}: вход ${port.type} для операции ${node.name}. ${port.required ? "Нужно подключить источник." : "Можно оставить без провода."}`
    : `${port.name}: ${port.type} input for ${node.name}. ${port.required ? "A source connection is required." : "The connection is optional."}`;
  if (!description) description = ru ? `Результат ${node.name} с типом ${port.type}. Доступен другим нодам на каждом сэмпле.` : `The ${port.type} result of ${node.name}, available to other nodes on every sample.`;
  const source = node.id === "biquad" && port.id === "coefficients" ? "Biquad Coefficients / Output" : sources[port.type];
  const destination = node.id === "biquad_coefficients" ? "Biquad / Coefficients" : destinations[port.type];
  const connection = direction === "input" ? `${source} → ${node.name} / ${port.name}` : `${node.name} / ${port.name} → ${destination}`;
  return { ...port, description, connection };
}
