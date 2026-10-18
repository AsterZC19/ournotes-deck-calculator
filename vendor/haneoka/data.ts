/** Minimal data helpers for Haneoka engine closure.
 * Derived from Haneoka data.ts (MPL-2.0).
 */

export type DataRow = Record<string, unknown>;

export const objectRow = (value: unknown): DataRow =>
  value && typeof value === "object" && !Array.isArray(value) ? (value as DataRow) : {};

export const dataRows = (value: unknown): DataRow[] =>
  (Array.isArray(value) ? value : Object.values(objectRow(value))).map(objectRow);

export const nativeRow = (value: unknown): DataRow => {
  const row = objectRow(value);
  return {
    ...Object.fromEntries(Object.entries(objectRow(row.raw)).map(([key, child]) => [key.replace(/^_/u, ""), child])),
    ...Object.fromEntries(Object.entries(row).filter(([key]) => key !== "raw")),
  };
};

export interface TeamBuilderData {
  identity?: { server: string; releaseId: string };
  [key: string]: unknown;
}
