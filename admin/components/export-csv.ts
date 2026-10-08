type Cell = string | number | boolean | null | undefined;

export function downloadCsv(filename: string, columns: string[], rows: Cell[][]) {
  const cell = (value: Cell) => {
    let text = String(value ?? "");
    if (typeof value === "string" && (/^\s*[=+\-@]/.test(text) || /^[\t\r\n]/.test(text))) text = "'" + text;
    return '"' + text.replace(/"/g, '""') + '"';
  };
  const csv = "\uFEFF" + [columns, ...rows].map(row => row.map(cell).join(";")).join("\r\n");
  const url = URL.createObjectURL(new Blob([csv], { type: "text/csv;charset=utf-8" }));
  const link = document.createElement("a");
  link.href = url; link.download = filename;
  document.body.append(link); link.click(); link.remove();
  window.setTimeout(() => URL.revokeObjectURL(url), 1000);
}
