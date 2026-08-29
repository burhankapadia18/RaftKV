import { render } from 'preact';

import { App } from './App';
import './styles/global.css';

const root = document.getElementById('root');
if (root === null) {
  throw new Error('#root is missing from index.html');
}
render(<App />, root);
