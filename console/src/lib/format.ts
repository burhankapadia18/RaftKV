export function formatInt(value: number): string {
  return value.toLocaleString('en-US');
}

export function formatBytes(value: number): string {
  if (value < 1024) return `${value} B`;
  const units = ['KiB', 'MiB', 'GiB', 'TiB'];
  let scaled = value / 1024;
  let unit = 0;
  while (scaled >= 1024 && unit < units.length - 1) {
    scaled /= 1024;
    unit += 1;
  }
  return `${scaled.toFixed(scaled < 10 ? 1 : 0)} ${units[unit]}`;
}
