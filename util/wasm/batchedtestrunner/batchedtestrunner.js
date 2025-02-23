// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only

import {
    AbortedError,
    ModuleLoader,
    ResourceFetcher,
    ResourceLocator,
} from './qwasmjsruntime.js';

import { parseQuery, EventSource } from './util.js';

class ProgramError extends Error {
    constructor(exitCode) {
        super(`The program reported an exit code of ${exitCode}`)
    }
}

class RunnerStatus {
    static Running = 'Running';
    static Completed = 'Completed';
    static Error = 'Error';
}

class TestStatus {
    static Pending = 'Pending';
    static Running = 'Running';
    static Completed = 'Completed';
    static Error = 'Error';
    static Crashed = 'Crashed';
}

// Represents the public API of the runner.
class WebApi {
    #results = new Map();
    #status = RunnerStatus.Running;
    #statusChangedEventPrivate;
    #testStatusChangedEventPrivate;
    #errorDetails;

    onStatusChanged =
        new EventSource((privateInterface) => this.#statusChangedEventPrivate = privateInterface);
    onTestStatusChanged =
        new EventSource((privateInterface) =>
            this.#testStatusChangedEventPrivate = privateInterface);

    // The callback receives the private interface of this object, meant not to be used by the
    // end user on the web side.
    constructor(receivePrivateInterface) {
        receivePrivateInterface({
            registerTest: testName => this.#registerTest(testName),
            setTestStatus: (testName, status) => this.#setTestStatus(testName, status),
            setTestResultData: (testName, testStatus, exitCode, textOutput) =>
                this.#setTestResultData(testName, testStatus, exitCode, textOutput),
            setTestRunnerStatus: status => this.#setTestRunnerStatus(status),
            setTestRunnerError: details => this.#setTestRunnerError(details),
        });
    }

    get results() { return this.#results; }
    get status() { return this.#status; }
    get errorDetails() { return this.#errorDetails; }

    #registerTest(testName) { this.#results.set(testName, { status: TestStatus.Pending }); }

    #setTestStatus(testName, status) {
        const testData = this.#results.get(testName);
        if (testData.status === status)
            return;
        this.#results.get(testName).status = status;
        this.#testStatusChangedEventPrivate.fireEvent(testName, status);
    }

    #setTestResultData(testName, testStatus, exitCode, textOutput) {
        const testData = this.#results.get(testName);
        const statusChanged = testStatus !== testData.status;
        testData.status = testStatus;
        testData.exitCode = exitCode;
        testData.textOutput = textOutput;
        if (statusChanged)
            this.#testStatusChangedEventPrivate.fireEvent(testName, testStatus);
    }

    #setTestRunnerStatus(status) {
        if (status === this.#status)
            return;
        this.#status = status;
        this.#statusChangedEventPrivate.fireEvent(status);
    }

    #setTestRunnerError(details) {
        this.#status = RunnerStatus.Error;
        this.#errorDetails = details;
        this.#statusChangedEventPrivate.fireEvent(RunnerStatus.Error);
    }
}

class BatchedTestRunner {
    static #TestBatchModuleName = 'test_batch';

    #loader;
    #privateWebApi;

    constructor(loader, privateWebApi) {
        this.#loader = loader;
        this.#privateWebApi = privateWebApi;
    }

    async #doRun(testName, testOutputFormat) {
        const module = await this.#loader.loadEmscriptenModule(
            BatchedTestRunner.#TestBatchModuleName,
            () => { }
        );

        const testsToExecute = testName ? [testName] : await this.#getTestClassNames(module);
        testsToExecute.forEach(testClassName => this.#privateWebApi.registerTest(testClassName));
        for (const testClassName of testsToExecute) {
            let result = {};
            this.#privateWebApi.setTestStatus(testClassName, TestStatus.Running);

            try {
                const LogToStdoutSpecialFilename = '-';
                result = await module.exec({
                    args: [testClassName, '-o', `${LogToStdoutSpecialFilename},${testOutputFormat}`],
                });

                if (result.exitCode < 0)
                    throw new ProgramError(result.exitCode);
                result.status = TestStatus.Completed;
            } catch (e) {
                result.status = e instanceof ProgramError ? TestStatus.Error : TestStatus.Crashed;
                result.stdout = e instanceof AbortedError ? e.stdout : result.stdout;
            }
            this.#privateWebApi.setTestResultData(
                testClassName, result.status, result.exitCode, result.stdout);
            }
        }

    async run(testName, testOutputFormat) {

        await this.#doRun(testName, testOutputFormat);
            this.#privateWebApi.setTestRunnerStatus(RunnerStatus.Completed);

    }

    async #getTestClassNames(module) {
        return (await module.exec()).stdout.trim().split(' ');
    }
}

(() => {
    let privateWebApi;
    window.qtTestRunner = new WebApi(privateApi => privateWebApi = privateApi);

    const parsed = parseQuery(location.search);
    const testName = parsed.get('qtestname');
    try {
        if (typeof testName !== 'undefined' && (typeof testName !== 'string' || testName === ''))
            throw new Error('The testName parameter is incorrect');

        const testOutputFormat = (() => {
            const format = parsed.get('qtestoutputformat') ?? 'txt';
            console.log(format);
            if (-1 === ['txt', 'xml', 'lightxml', 'junitxml', 'tap'].indexOf(format))
                throw new Error(`Bad file format: ${format}`);
            return format;
        })();

        const resourceLocator = new ResourceLocator('');
        const testRunner = new BatchedTestRunner(
            new ModuleLoader(new ResourceFetcher(resourceLocator), resourceLocator),
            privateWebApi
        );

        testRunner.run(testName, testOutputFormat);
    } catch (e) {
        privateWebApi.setTestRunnerError(e.message);
    }
})();
