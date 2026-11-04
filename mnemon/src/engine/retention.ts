const BASE: Record<number, number> = {
  5: 1,
  4: 0.8,
  3: 0.5,
  2: 0.3,
  1: 0.15,
};

export function effectiveImportance(input: {
  importance: number;
  accessCount: number;
  daysSinceAccess: number;
  edgeCount: number;
}): number {
  const base = BASE[input.importance] ?? 0.15;
  const accessFactor = Math.max(1, Math.log1p(input.accessCount));
  const decayFactor = 0.5 ** (input.daysSinceAccess / 30);
  const edgeFactor = 1 + 0.1 * Math.min(input.edgeCount, 5);
  return base * accessFactor * decayFactor * edgeFactor;
}
