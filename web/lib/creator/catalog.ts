import registry from "./registry.json";
import { nodeCopy } from "./node-copy";
import { l, type CreatorNode, type Localized, type PortType, type RegistryNode } from "./types";

export const creatorNodes: CreatorNode[] = (registry as RegistryNode[]).map(node => {
  const text = nodeCopy[node.id];
  if (!text) throw new Error(`Missing Creator documentation: ${node.id}`);
  return { ...node, ...text };
});
export const findNode = (id: string) => creatorNodes.find(node => node.id === id);
export const categories: Record<string, Localized> = {
  Routing: l("Маршрутизация", "Routing"), Effects: l("Эффекты", "Effects"), Generators: l("Генераторы", "Generators"),
  Modulation: l("Модуляция", "Modulation"), Analysis: l("Анализ", "Analysis"), Signal: l("Аудиосигнал", "Signal"),
  Math: l("Математика", "Math"), Logic: l("Логика", "Logic"), Memory: l("Память и время", "Memory & time"),
  Filters: l("Фильтры", "Filters"), Shaping: l("Форма сигнала", "Shaping"), Collections: l("Коллекции", "Collections"),
  Custom: l("Собственные ноды", "Custom nodes"), Code: l("C++", "C++"),
};
export const portTypes: { id: PortType; name: string; color: string; description: Localized; example: string }[] = [
  { id: "audio", name: "Audio", color: "#63d6b6", description: l("Звуковой поток: mono или stereo. Соединяет вход, эффекты и выход модуля.", "A mono or stereo sound stream. Connects the input, effects and module output."), example: "Input → Gain → Output" },
  { id: "number", name: "Number", color: "#8abaff", description: l("Число на каждом сэмпле: частота, усиление, огибающая или другая модуляция.", "A number on every sample: frequency, gain, an envelope or another modulation signal."), example: "LFO → Gain / Gain" },
  { id: "gate", name: "Gate", color: "#d7a1ef", description: l("Логическое состояние. Переключает ветви, запускает события и сбрасывает генераторы.", "A logical state. Switches branches, triggers events and resets generators."), example: "Compare → Sample & Hold / Trigger" },
  { id: "function", name: "Function", color: "#efc078", description: l("Вызываемая C++-функция. Обе стороны должны иметь одинаковую полную сигнатуру; это не поток Audio.", "A callable C++ function. Both sides must have exactly the same signature; this is not an Audio stream."), example: "saturate() → process / saturate" },
  { id: "integer", name: "Integer", color: "#baca74", description: l("Целое число для индексов и счётчиков. Для Number нужен явный преобразователь.", "An integer for indices and counters. Use an explicit converter to connect it to Number."), example: "Counter → Get Element / Index" },
  { id: "array", name: "Array", color: "#e6a878", description: l("Массив чисел фиксированной ёмкости: коэффициенты фильтра, таблица или вектор.", "A fixed-capacity number array: filter coefficients, a lookup table or a vector."), example: "Biquad Coefficients → Biquad" },
  { id: "list", name: "List", color: "#ec95b0", description: l("Список переменной длины внутри заранее выделенной памяти. Поддерживает добавление и удаление.", "A variable-length list within preallocated storage. Supports appending and removing elements."), example: "List → Append → Length" },
  { id: "buffer", name: "Buffer", color: "#79ced7", description: l("Ссылка на историю аудиозадержки. Передаёт подготовленный буфер в ноды чтения.", "A reference to audio delay history. Passes prepared storage to delay-reading nodes."), example: "Delay Buffer → Read Delay Tap" },
];
export const dynamicNodeIds = new Set(["interface", "cpp_function", "subgraph", "subgraph_input", "subgraph_output", "history", "wire", "length", "get", "set", "clear", "sum", "collection_min", "collection_max", "map", "reduce"]);
export const creatorVersion = "0.3.2 beta";
