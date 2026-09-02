import type { EmbeddingProvider } from "./embedding-provider.js";
import { MnemonEmbeddingError } from "./errors.js";

export const NOMIC_EMBED_TEXT_DIMENSIONS = 768;

export type EmbeddingProtocol = "llamacpp" | "openai" | "ollama";

export interface HttpEmbeddingProviderOptions {
  endpoint?: string;
  model?: string;
  dimensions?: number;
  apiKey?: string;
  protocol?: EmbeddingProtocol;
}

type Purpose = "document" | "query";

interface ProtocolSpec {
  label: string;
  defaultEndpoint: string;
  defaultDimensions: number;
  prefix: (purpose: Purpose) => string;
  encodingFormat?: "float";
  truncate: boolean;
  sendDimensions: boolean;
  sendAuth: boolean;
  strictDimensions: boolean;
  url: (endpoint: string) => string;
  parse: (payload: unknown) => number[] | undefined;
}

function trimSlash(url: string): string {
  return url.replace(/\/$/u, "");
}

function asFiniteVector(vector: unknown): number[] | undefined {
  if (!Array.isArray(vector) || vector.length === 0) {
    return undefined;
  }
  const out: number[] = [];
  for (const n of vector) {
    if (typeof n !== "number" || !Number.isFinite(n)) {
      return undefined;
    }
    out.push(n);
  }
  return out;
}

function openaiVector(payload: unknown): number[] | undefined {
  if (typeof payload !== "object" || payload === null || !("data" in payload)) {
    return undefined;
  }
  return asFiniteVector((payload as { data?: Array<{ embedding?: unknown }> }).data?.[0]?.embedding);
}

function ollamaVector(payload: unknown): number[] | undefined {
  if (typeof payload !== "object" || payload === null || !("embeddings" in payload)) {
    return undefined;
  }
  return asFiniteVector((payload as { embeddings?: unknown[] }).embeddings?.[0]);
}

const PROTOCOL: Record<EmbeddingProtocol, ProtocolSpec> = {
  llamacpp: {
    label: "llama.cpp",
    defaultEndpoint: "http://127.0.0.1:8080",
    defaultDimensions: NOMIC_EMBED_TEXT_DIMENSIONS,
    prefix: (purpose) => (purpose === "query" ? "search_query: " : "search_document: "),
    encodingFormat: "float",
    truncate: true,
    sendDimensions: true,
    sendAuth: true,
    strictDimensions: false,
    url: (endpoint) => `${endpoint}/v1/embeddings`,
    parse: openaiVector,
  },
  openai: {
    label: "openai",
    defaultEndpoint: "http://127.0.0.1:8080",
    defaultDimensions: 1536,
    prefix: () => "",
    truncate: false,
    sendDimensions: true,
    sendAuth: true,
    strictDimensions: true,
    url: (endpoint) => (endpoint.endsWith("/v1") ? `${endpoint}/embeddings` : `${endpoint}/v1/embeddings`),
    parse: openaiVector,
  },
  ollama: {
    label: "ollama",
    defaultEndpoint: "http://127.0.0.1:11434",
    defaultDimensions: NOMIC_EMBED_TEXT_DIMENSIONS,
    prefix: () => "",
    truncate: false,
    sendDimensions: false,
    sendAuth: false,
    strictDimensions: true,
    url: (endpoint) => `${endpoint}/api/embed`,
    parse: ollamaVector,
  },
};

function resolveDimensions(explicit: number | undefined, protocol: EmbeddingProtocol): number {
  if (explicit !== undefined) {
    if (!Number.isInteger(explicit) || explicit <= 0) {
      throw new MnemonEmbeddingError("dimensions must be a positive integer");
    }
    return explicit;
  }
  const raw = process.env.MNEMON_EMBED_DIMENSIONS;
  if (raw && raw.length > 0) {
    const parsed = Number(raw);
    if (!Number.isInteger(parsed) || parsed <= 0) {
      throw new MnemonEmbeddingError("MNEMON_EMBED_DIMENSIONS must be a positive integer");
    }
    return parsed;
  }
  return PROTOCOL[protocol].defaultDimensions;
}

/** HTTP embeddings. llama.cpp prefixes nomic text; Ollama matches the Go `/api/embed` client. */
export class HttpEmbeddingProvider implements EmbeddingProvider {
  readonly model: string;
  readonly dimensions: number;
  readonly protocol: EmbeddingProtocol;
  private readonly endpoint: string;
  private readonly apiKey?: string;

  constructor(options: HttpEmbeddingProviderOptions = {}) {
    this.protocol = options.protocol ?? "llamacpp";
    const spec = PROTOCOL[this.protocol];
    this.endpoint = trimSlash(
      options.endpoint ?? process.env.MNEMON_EMBED_ENDPOINT ?? spec.defaultEndpoint,
    );
    this.model = options.model ?? process.env.MNEMON_EMBED_MODEL ?? "nomic-embed-text";
    this.dimensions = resolveDimensions(options.dimensions, this.protocol);
    const key = options.apiKey ?? process.env.MNEMON_EMBED_API_KEY;
    this.apiKey = key && key.length > 0 ? key : undefined;
  }

  async embed(text: string, purpose: Purpose): Promise<readonly number[]> {
    const spec = PROTOCOL[this.protocol];
    const body: Record<string, unknown> = {
      input: spec.prefix(purpose) + text,
      model: this.model,
    };
    if (spec.sendDimensions || process.env.MNEMON_EMBED_DIMENSIONS) {
      body.dimensions = this.dimensions;
    }
    if (spec.encodingFormat) {
      body.encoding_format = spec.encodingFormat;
    }

    const headers: Record<string, string> = { "content-type": "application/json" };
    if (spec.sendAuth && this.apiKey) {
      headers.authorization = `Bearer ${this.apiKey}`;
    }

    let response: Response;
    try {
      response = await fetch(spec.url(this.endpoint), {
        method: "POST",
        headers,
        body: JSON.stringify(body),
      });
    } catch (error) {
      throw new MnemonEmbeddingError(`${spec.label} embedding request failed`, { cause: error });
    }
    if (!response.ok) {
      throw new MnemonEmbeddingError(`${spec.label} embedding failed: ${response.status}`);
    }
    let payload: unknown;
    try {
      payload = await response.json();
    } catch (error) {
      throw new MnemonEmbeddingError(`${spec.label} returned invalid JSON`, { cause: error });
    }
    const vector = spec.parse(payload);
    if (!vector) {
      throw new MnemonEmbeddingError(`${spec.label} returned no embedding`);
    }
    if (spec.truncate && vector.length > this.dimensions) {
      return vector.slice(0, this.dimensions);
    }
    if ((spec.truncate || spec.strictDimensions) && vector.length !== this.dimensions) {
      throw new MnemonEmbeddingError(
        `embedding dimension mismatch: requested ${this.dimensions}, received ${vector.length}`,
      );
    }
    return vector;
  }
}

export class LlamaCppEmbeddingProvider extends HttpEmbeddingProvider {
  constructor(options: Omit<HttpEmbeddingProviderOptions, "protocol"> = {}) {
    super({ ...options, protocol: "llamacpp" });
  }
}

export class OpenAIEmbeddingProvider extends HttpEmbeddingProvider {
  constructor(options: Omit<HttpEmbeddingProviderOptions, "protocol"> = {}) {
    super({ ...options, protocol: "openai" });
  }
}

export class OllamaEmbeddingProvider extends HttpEmbeddingProvider {
  constructor(options: Omit<HttpEmbeddingProviderOptions, "protocol"> = {}) {
    super({ ...options, protocol: "ollama" });
  }
}
