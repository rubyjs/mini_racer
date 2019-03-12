module.exports = {
  mode: "production",
  entry: "./index.jsx",
  // This will put our source maps in a seperate .map file which we will read
  // later
  devtool: "source-map",
  module: {
    rules: [
      {
        test: /\.jsx?$/,
        exclude: /node_modules/,
        use: {
          loader: "babel-loader"
        }
      }
    ]
  },
  output: {
    library: "webpackLib",
    libraryTarget: "umd",
    // This is necessary to make webpack not define globals on the non-existent
    // 'window' object
    globalObject: "this"
  }
};
