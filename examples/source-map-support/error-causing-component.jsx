function throwSomeError() {
  throw new Error(
    "^^ Look! These stack traces map to the actual source code :)"
  );
}

export const ErrorCausingComponent = () => {
  throwSomeError();
};
