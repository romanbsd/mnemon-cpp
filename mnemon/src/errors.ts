export class MnemonError extends Error {
  override readonly name: string = "MnemonError";

  constructor(message: string, options?: ErrorOptions) {
    super(message, options);
  }
}

export class MnemonValidationError extends MnemonError {
  override readonly name = "MnemonValidationError";

  constructor(
    message: string,
    readonly field: string,
    readonly code: string,
  ) {
    super(message);
  }
}

export class MnemonConfigurationError extends MnemonError {
  override readonly name = "MnemonConfigurationError";
}

export class MnemonDatabaseError extends MnemonError {
  override readonly name = "MnemonDatabaseError";
  readonly code?: string;

  constructor(message: string, options?: ErrorOptions & { code?: string }) {
    super(message, options);
    this.code = options?.code;
  }
}

export class MnemonEmbeddingError extends MnemonError {
  override readonly name = "MnemonEmbeddingError";
}

export class MnemonNotFoundError extends MnemonError {
  override readonly name = "MnemonNotFoundError";

  constructor(
    message: string,
    readonly id?: string,
  ) {
    super(message);
  }
}
