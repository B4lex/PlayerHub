package admin

import (
	"net/http"
	"os"
	"path"
	"path/filepath"
	"strings"
)

const notBuilt = `<!doctype html>
<title>Admin panel not built</title>
<body style="font: 16px system-ui; max-width: 40rem; margin: 4rem auto; padding: 0 1rem">
<h1>Admin panel not built</h1>
<p>Build it once with <code>npm install &amp;&amp; npm run build</code> in
<code>server/admin-ui</code>, or run <code>npm run dev</code> there and open the
address it prints. The REST API under <code>/api</code> works meanwhile.</p>
</body>`

// UI serves the built panel in directory at /admin/. Unknown paths without an
// extension get index.html, for client-side routes.
func UI(directory string) http.Handler {
	files := http.StripPrefix("/admin", http.FileServer(http.Dir(directory)))
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if _, err := os.Stat(filepath.Join(directory, "index.html")); err != nil {
			w.Header().Set("Content-Type", "text/html; charset=utf-8")
			w.WriteHeader(http.StatusServiceUnavailable)
			w.Write([]byte(notBuilt))
			return
		}
		relative := strings.TrimPrefix(path.Clean(r.URL.Path), "/admin")
		if relative != "" && relative != "/" && path.Ext(relative) == "" {
			if _, err := os.Stat(filepath.Join(directory, filepath.FromSlash(relative))); err != nil {
				http.ServeFile(w, r, filepath.Join(directory, "index.html"))
				return
			}
		}
		files.ServeHTTP(w, r)
	})
}
