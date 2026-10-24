// Binary arrays arrive inside JSON as base64 (docs/HLSD/engine.md).

export function decodeBase64(text: string): Uint8Array {
  const binary = atob(text);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i += 1) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

export const toFloat32 = (text: string): Float32Array => new Float32Array(decodeBase64(text).buffer);
export const toUint32 = (text: string): Uint32Array => new Uint32Array(decodeBase64(text).buffer);
export const toUint16 = (text: string): Uint16Array => new Uint16Array(decodeBase64(text).buffer);
export const toUint8 = (text: string): Uint8Array => decodeBase64(text);
