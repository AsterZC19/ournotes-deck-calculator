import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

export async function resolve(specifier, context, nextResolve) {
  let filepath = null;
  if (specifier.startsWith('./') || specifier.startsWith('../')) {
    const parent = context.parentURL ? fileURLToPath(context.parentURL) : process.cwd();
    filepath = path.resolve(path.dirname(parent), specifier);
  } else if (specifier.startsWith('/')) {
    filepath = specifier;
  } else if (specifier.startsWith('file://')) {
    filepath = fileURLToPath(specifier);
  }

  if (filepath) {
    try {
      if (fs.existsSync(filepath + '.ts')) {
        return { url: pathToFileURL(filepath + '.ts').href, shortCircuit: true };
      }
      if (fs.existsSync(filepath + '.js')) {
        return { url: pathToFileURL(filepath + '.js').href, shortCircuit: true };
      }
      if (fs.existsSync(path.join(filepath, 'index.ts'))) {
        return { url: pathToFileURL(path.join(filepath, 'index.ts')).href, shortCircuit: true };
      }
      if (fs.existsSync(path.join(filepath, 'index.js'))) {
        return { url: pathToFileURL(path.join(filepath, 'index.js')).href, shortCircuit: true };
      }
      if (fs.existsSync(filepath) && fs.statSync(filepath).isFile()) {
        return { url: pathToFileURL(filepath).href, shortCircuit: true };
      }
    } catch {
      // ignore
    }
  }

  return nextResolve(specifier, context);
}
