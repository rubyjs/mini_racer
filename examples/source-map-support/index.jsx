import { ErrorCausingComponent } from "./error-causing-component.jsx";

if (process.env.NODE_ENV === "production") {
  require("source-map-support").install({
    // We tell the source-map-support package to retrieve our source maps by
    // calling the `readSourceMap` global function, which we attached to the
    // miniracer context
    retrieveSourceMap: filename => {
      return {
        url: filename,
        map: readSourceMap(filename)
      };
    }
  });
}

// We expose this function so we can call it later
export function renderComponent() {
  ErrorCausingComponent();
}
