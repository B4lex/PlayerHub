import { Navigate, Route, Routes } from "react-router-dom";
import styles from "./App.module.css";
import { Sidebar } from "./components/Sidebar";
import { pages } from "./pages";

export function App() {
  return (
    <div className={styles.shell}>
      <Sidebar pages={pages} />
      <main className={styles.main}>
        <div className={styles.content}>
          <Routes>
            {pages.map((page) => (
              <Route key={page.id} path={page.path} element={page.element} />
            ))}
            <Route path="*" element={<Navigate to="/" replace />} />
          </Routes>
        </div>
      </main>
    </div>
  );
}
