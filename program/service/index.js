import express from "express";
import multer from "multer";
import tar from 'tar-stream';
import fs from 'fs';
import { exec } from 'child_process';
import swaggerUi from 'swagger-ui-express';
import swaggerJsdoc from 'swagger-jsdoc';

// Swagger definition
const swaggerOptions = {
  swaggerDefinition: {
    openapi: '3.0.0',
    info: {
      title: 'Docker Build API',
      version: '1.0.0',
      description: 'API for building projects inside Docker using REST'
    },
    servers: [
      {
        url: 'http://localhost:3000',
        description: 'Local server'
      }
    ]
  },
  apis: ['./index.js'], // Path to your API docs (this file)
};

const swaggerDocs = swaggerJsdoc(swaggerOptions);

let app = express();
const storage = multer.memoryStorage();
const upload = multer({ storage: storage });

// Serve Swagger UI at '/api-docs'
app.use('/api-docs', swaggerUi.serve, swaggerUi.setup(swaggerDocs));

// Redirect root path '/' to '/api-docs'
app.get('/', (req, res) => {
  res.redirect('/api-docs');
});

/**
 * @openapi
 * /build/{language}:
 *   post:
 *     summary: Upload files to build in the specified language
 *     parameters:
 *       - in: path
 *         name: language
 *         schema:
 *           type: string
 *         required: true
 *         default: cpp
 *         description: Language to build (e.g., cpp)
 *     requestBody:
 *       required: true
 *       content:
 *         multipart/form-data:
 *           schema:
 *             type: object
 *             properties:
 *               file:
 *                 type: string
 *                 format: binary
 *     responses:
 *       200:
 *         description: Successfully built the project
 *       400:
 *         description: Invalid input or no files
 *       404:
 *         description: Unsupported language
 */
app.post('/build/:language', upload.any(), async (req, res) => {
    let image = ({
        cpp: 'cpp_compiler' // Assume the Docker is built with this compiler
    })[req.params.language];

    if (!image) {
        res.status(404).send('Unsupported Language');
        return;
    }

    const pack = tar.pack();

    // Save files to /code directory
    if (!fs.existsSync('./code')) {
        fs.mkdirSync('./code', { recursive: true });
    }

    for (let file of req.files) {
        let filePath = './code/' + file.originalname;
        fs.writeFileSync(filePath, file.buffer);
    }

    let names = req.files.filter(x => /[.]c.?.?$/.test(x.originalname)).map(x => './code/' + x.originalname);
    if (names.length === 0) {
        res.status(400).send('No C/C++ files found.');
        return;
    }

    // Execute the build command inside the container
    exec(`/usr/api/build.sh -v -o /tmp/out.elf ${names.join(' ')} 2>/tmp/stderr 1>/tmp/stdout`, (err, stdout, stderr) => {
        if (err) {
            res.status(500).send('Build failed: ' + stderr);
            return;
        }

        // Respond with the result (binary or logs)
        fs.readFile('./tmp/out.elf', (err, data) => {
            if (err) {
                res.status(500).send('Error retrieving the output file.');
            } else {
                res.set('Content-Type', 'application/octet-stream');
                res.send(data);
            }
        });
    });
});

let server = app.listen(process.env.PORT || 3000, () => {
    console.log(`Listening on ${server.address().port}`);
});